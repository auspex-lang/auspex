#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sql_engine.h"

/* -------------------------------------------------------------------------- */
/* Page-based B+ tree over the pager.                                         */
/*                                                                            */
/* One node per page. Leaf pages hold (key -> row locator) entries sorted by   */
/* key and are linked left-to-right for range scans; internal pages hold       */
/* separator keys (copies of the first key of their right child). Duplicate    */
/* keys are allowed.                                                           */
/*                                                                            */
/* Keys are encoded to a fixed-width order-preserving byte string:             */
/*   byte 0: tag (0 NULL, 1 int, 2 float, 3 string)                            */
/*   int:    4-byte big-endian of value ^ 0x80000000 (bools share this space,  */
/*           encoded as 0 or 1)                                                */
/*   float:  8-byte big-endian IEEE-754 with the standard sign transform       */
/*   string: first BTREE_STRING_KEY_BYTES bytes, zero-padded (longer strings   */
/*           share a truncated key, so scans may yield extra candidates,       */
/*           never fewer - truncation is monotone, which is what lets the SQL   */
/*           layer widen a truncated range bound instead of losing rows)       */
/* Ordering: NULL < int < float < string, numeric inside the int/float key     */
/* spaces, bytewise inside the string space.                                   */
/*                                                                            */
/* Deletion keeps every node but the root at least half full: an underfull    */
/* node borrows one entry from a sibling, or merges with it and frees a page.  */
/* When merges empty the root of its separators the tree loses a level, so     */
/* root_page can change - callers that persist it must re-read it afterwards.  */
/* -------------------------------------------------------------------------- */

#define BTREE_KEY_SIZE        (1 + BTREE_STRING_KEY_BYTES)
#define BTREE_LEAF_MAX        90
#define BTREE_INTERNAL_MAX    99

/* Minimum occupancy for every node but the root. Deletion redistributes or
   merges to restore it. */
#define BTREE_LEAF_MIN        (BTREE_LEAF_MAX / 2)
#define BTREE_INTERNAL_MIN    (BTREE_INTERNAL_MAX / 2)

#define BTREE_TYPE_LEAF       1
#define BTREE_TYPE_INTERNAL   2

/* Internal page layout: byte 0 type, u16 nkeys at 2, (nkeys+1) int32 child
   page numbers at offset 8, separator keys at a fixed offset. */
#define BTREE_INTERNAL_KEYS_OFF (8 + 4 * (BTREE_INTERNAL_MAX + 1))

/* Leaf page layout: byte 0 type, u16 nkeys at 2, int32 next at 4, then
   (key, row_page, row_offset) triples from offset 8. */
#define BTREE_LEAF_ENTRY_SIZE (BTREE_KEY_SIZE + 2 * (int)sizeof(int32_t))

/* The fanouts above are hand-tuned to BTREE_KEY_SIZE. Fail the build rather
   than silently overrun a page if the key width ever changes. */
typedef char btree_leaf_fits_a_page[
    (8 + BTREE_LEAF_MAX * BTREE_LEAF_ENTRY_SIZE <= PAGE_SIZE) ? 1 : -1];
typedef char btree_internal_fits_a_page[
    (BTREE_INTERNAL_KEYS_OFF + BTREE_INTERNAL_MAX * BTREE_KEY_SIZE <= PAGE_SIZE) ? 1 : -1];

struct BTree {
    Pager* pager;
    int    root_page;
};

/* In memory a node holds one entry more than a page does: an insert lands
   before the overfull node is split, so an internal node briefly has
   BTREE_INTERNAL_MAX + 1 keys and BTREE_INTERNAL_MAX + 2 children. */
typedef struct {
    int     is_leaf;
    int     nkeys;
    uint8_t keys[BTREE_INTERNAL_MAX + 1][BTREE_KEY_SIZE];
    int32_t children[BTREE_INTERNAL_MAX + 2]; /* internal nodes */
    int32_t row_pages[BTREE_INTERNAL_MAX + 1];    /* leaf entries */
    int32_t row_offsets[BTREE_INTERNAL_MAX + 1];  /* leaf entries */
    int32_t next;                             /* leaf link */
} BtNode;

/* -------------------------------------------------------------------------- */
/* Key encoding                                                               */
/* -------------------------------------------------------------------------- */

static void encode_key(const Cell* cell, uint8_t* out) {
    memset(out, 0, BTREE_KEY_SIZE);
    if (cell == NULL || cell->type == VAL_NULL) {
        out[0] = 0;
        return;
    }
    if (cell->type == VAL_INT || cell->type == VAL_BOOL) {
        /* A bool rides in the int key space as 0 or 1: false orders before
           true, and no new tag is needed. A column is one type, so the spaces
           never mix in a single index. */
        uint32_t u = ((uint32_t)(int32_t)cell->as.as_int) ^ 0x80000000u;
        out[0] = 1;
        out[1] = (uint8_t)(u >> 24);
        out[2] = (uint8_t)(u >> 16);
        out[3] = (uint8_t)(u >> 8);
        out[4] = (uint8_t)u;
        return;
    }
    if (cell->type == VAL_FLOAT) {
        uint64_t u = 0;
        memcpy(&u, &cell->as.as_float, sizeof(u));
        /* Order-preserving transform: negatives become bit-complements,
           non-negatives get the sign bit set. */
        if (u & 0x8000000000000000ULL) {
            u = ~u;
        } else {
            u |= 0x8000000000000000ULL;
        }
        out[0] = 2;
        for (int i = 0; i < 8; i++) {
            out[1 + i] = (uint8_t)(u >> (56 - 8 * i));
        }
        return;
    }
    /* Dates and timestamps are canonical fixed-width text, well inside the
       significant prefix, so they share the string key space and order
       chronologically there. A string literal written for a date column
       therefore encodes to the same key as the stored date. */
    out[0] = 3;
    int is_text = cell->type == VAL_STRING || cell->type == VAL_DATE ||
                  cell->type == VAL_TIMESTAMP;
    const char* s = (is_text && cell->as.as_string != NULL)
        ? cell->as.as_string : "";
    size_t n = strlen(s);
    if (n > BTREE_STRING_KEY_BYTES) n = BTREE_STRING_KEY_BYTES;
    memcpy(out + 1, s, n);
}

/* -------------------------------------------------------------------------- */
/* Node (de)serialization                                                     */
/* -------------------------------------------------------------------------- */

static int node_load(Pager* pager, int page_num, BtNode* node) {
    uint8_t page[PAGE_SIZE];
    pager_read_page(pager, page_num, page);

    memset(node, 0, sizeof(*node));
    uint8_t type = page[0];
    if (type != BTREE_TYPE_LEAF && type != BTREE_TYPE_INTERNAL) return 0;
    uint16_t nkeys = 0;
    memcpy(&nkeys, page + 2, sizeof(nkeys));
    int max = type == BTREE_TYPE_LEAF ? BTREE_LEAF_MAX : BTREE_INTERNAL_MAX;
    if ((int)nkeys > max) return 0;

    node->is_leaf = type == BTREE_TYPE_LEAF;
    node->nkeys = (int)nkeys;

    if (node->is_leaf) {
        memcpy(&node->next, page + 4, sizeof(node->next));
        int offset = 8;
        for (int i = 0; i < node->nkeys; i++) {
            memcpy(node->keys[i], page + offset, BTREE_KEY_SIZE);
            offset += BTREE_KEY_SIZE;
            memcpy(&node->row_pages[i], page + offset, sizeof(int32_t));
            offset += sizeof(int32_t);
            memcpy(&node->row_offsets[i], page + offset, sizeof(int32_t));
            offset += sizeof(int32_t);
        }
    } else {
        int offset = 8;
        for (int i = 0; i <= node->nkeys; i++) {
            memcpy(&node->children[i], page + offset, sizeof(int32_t));
            offset += sizeof(int32_t);
        }
        offset = BTREE_INTERNAL_KEYS_OFF;
        for (int i = 0; i < node->nkeys; i++) {
            memcpy(node->keys[i], page + offset, BTREE_KEY_SIZE);
            offset += BTREE_KEY_SIZE;
        }
    }
    return 1;
}

static void node_store(Pager* pager, int page_num, const BtNode* node) {
    uint8_t page[PAGE_SIZE];
    memset(page, 0, PAGE_SIZE);

    page[0] = (uint8_t)(node->is_leaf ? BTREE_TYPE_LEAF : BTREE_TYPE_INTERNAL);
    uint16_t nkeys = (uint16_t)node->nkeys;
    memcpy(page + 2, &nkeys, sizeof(nkeys));

    if (node->is_leaf) {
        memcpy(page + 4, &node->next, sizeof(node->next));
        int offset = 8;
        for (int i = 0; i < node->nkeys; i++) {
            memcpy(page + offset, node->keys[i], BTREE_KEY_SIZE);
            offset += BTREE_KEY_SIZE;
            memcpy(page + offset, &node->row_pages[i], sizeof(int32_t));
            offset += sizeof(int32_t);
            memcpy(page + offset, &node->row_offsets[i], sizeof(int32_t));
            offset += sizeof(int32_t);
        }
    } else {
        int offset = 8;
        for (int i = 0; i <= node->nkeys; i++) {
            memcpy(page + offset, &node->children[i], sizeof(int32_t));
            offset += sizeof(int32_t);
        }
        offset = BTREE_INTERNAL_KEYS_OFF;
        for (int i = 0; i < node->nkeys; i++) {
            memcpy(page + offset, node->keys[i], BTREE_KEY_SIZE);
            offset += BTREE_KEY_SIZE;
        }
    }
    pager_write_page(pager, page_num, page);
}

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* -------------------------------------------------------------------------- */

BTree* btree_create(Pager* pager) {
    if (pager == NULL) return NULL;
    BTree* tree = calloc(1, sizeof(BTree));
    if (tree == NULL) return NULL;
    tree->pager = pager;

    int root = pager_allocate_page(pager);
    if (root < 0) {
        free(tree);
        return NULL;
    }
    BtNode node;
    memset(&node, 0, sizeof(node));
    node.is_leaf = 1;
    node.nkeys = 0;
    node.next = 0;
    node_store(pager, root, &node);
    tree->root_page = root;
    return tree;
}

BTree* btree_open(Pager* pager, int root_page) {
    if (pager == NULL || root_page <= 0) return NULL;
    BTree* tree = calloc(1, sizeof(BTree));
    if (tree == NULL) return NULL;
    tree->pager = pager;
    tree->root_page = root_page;
    return tree;
}

void btree_destroy(BTree* tree) {
    free(tree);
}

int btree_root_page(BTree* tree) {
    return tree != NULL ? tree->root_page : 0;
}

static void free_pages_rec(Pager* pager, int page_num) {
    BtNode node;
    if (!node_load(pager, page_num, &node)) return;
    if (!node.is_leaf) {
        for (int i = 0; i <= node.nkeys; i++) {
            if (node.children[i] > 0) {
                free_pages_rec(pager, node.children[i]);
            }
        }
    }
    pager_free_page(pager, page_num);
}

void btree_free_pages(BTree* tree) {
    if (tree == NULL || tree->root_page <= 0) return;
    free_pages_rec(tree->pager, tree->root_page);
}

static int stats_rec(Pager* pager, int page_num, BTreeStats* out, int depth) {
    BtNode node;
    if (!node_load(pager, page_num, &node)) return 0;
    out->node_count++;
    if (depth > out->height) out->height = depth;
    if (node.is_leaf) {
        out->leaf_count++;
        out->entry_count += node.nkeys;
        return 1;
    }
    for (int i = 0; i <= node.nkeys; i++) {
        if (node.children[i] <= 0) return 0;
        if (!stats_rec(pager, node.children[i], out, depth + 1)) return 0;
    }
    return 1;
}

int btree_stats(BTree* tree, BTreeStats* out) {
    if (tree == NULL || out == NULL || tree->root_page <= 0) return 0;
    memset(out, 0, sizeof(*out));
    return stats_rec(tree->pager, tree->root_page, out, 1);
}

/* -------------------------------------------------------------------------- */
/* Insert                                                                     */
/* -------------------------------------------------------------------------- */

/* Inserts (key, locator) into the subtree rooted at page_num. On a split,
   *did_split is set, promote_key receives the separator to copy up, and
   promote_page the new right-hand page. */
static int insert_rec(BTree* tree, int page_num, const uint8_t* key,
                      int row_page, int row_offset,
                      uint8_t* promote_key, int32_t* promote_page,
                      int* did_split) {
    *did_split = 0;
    BtNode node;
    if (!node_load(tree->pager, page_num, &node)) return 0;

    if (node.is_leaf) {
        int pos = 0;
        while (pos < node.nkeys && memcmp(node.keys[pos], key, BTREE_KEY_SIZE) <= 0) {
            pos++;
        }
        for (int i = node.nkeys; i > pos; i--) {
            memcpy(node.keys[i], node.keys[i - 1], BTREE_KEY_SIZE);
            node.row_pages[i] = node.row_pages[i - 1];
            node.row_offsets[i] = node.row_offsets[i - 1];
        }
        memcpy(node.keys[pos], key, BTREE_KEY_SIZE);
        node.row_pages[pos] = row_page;
        node.row_offsets[pos] = row_offset;
        node.nkeys++;

        if (node.nkeys <= BTREE_LEAF_MAX) {
            node_store(tree->pager, page_num, &node);
            return 1;
        }

        /* Split: left keeps [0, split), right gets [split, nkeys). */
        int split = node.nkeys / 2;
        BtNode right;
        memset(&right, 0, sizeof(right));
        right.is_leaf = 1;
        right.nkeys = node.nkeys - split;
        for (int i = 0; i < right.nkeys; i++) {
            memcpy(right.keys[i], node.keys[split + i], BTREE_KEY_SIZE);
            right.row_pages[i] = node.row_pages[split + i];
            right.row_offsets[i] = node.row_offsets[split + i];
        }
        node.nkeys = split;
        right.next = node.next;

        int new_page = pager_allocate_page(tree->pager);
        if (new_page < 0) return 0;
        node.next = new_page;
        node_store(tree->pager, page_num, &node);
        node_store(tree->pager, new_page, &right);

        memcpy(promote_key, right.keys[0], BTREE_KEY_SIZE);
        *promote_page = new_page;
        *did_split = 1;
        return 1;
    }

    /* Internal node: descend into the first child whose separator is >= key
       (leftmost candidate, so duplicate separators stay reachable). */
    int i = 0;
    while (i < node.nkeys && memcmp(key, node.keys[i], BTREE_KEY_SIZE) > 0) {
        i++;
    }
    uint8_t child_key[BTREE_KEY_SIZE];
    int32_t child_page = 0;
    int child_split = 0;
    if (!insert_rec(tree, node.children[i], key, row_page, row_offset,
                    child_key, &child_page, &child_split)) {
        return 0;
    }
    if (!child_split) return 1;

    for (int j = node.nkeys; j > i; j--) {
        memcpy(node.keys[j], node.keys[j - 1], BTREE_KEY_SIZE);
        node.children[j + 1] = node.children[j];
    }
    memcpy(node.keys[i], child_key, BTREE_KEY_SIZE);
    node.children[i + 1] = child_page;
    node.nkeys++;

    if (node.nkeys <= BTREE_INTERNAL_MAX) {
        node_store(tree->pager, page_num, &node);
        return 1;
    }

    /* Split internal: the median separator moves up (not copied). */
    int mid = node.nkeys / 2;
    BtNode right;
    memset(&right, 0, sizeof(right));
    right.is_leaf = 0;
    right.nkeys = node.nkeys - mid - 1;
    for (int j = 0; j < right.nkeys; j++) {
        memcpy(right.keys[j], node.keys[mid + 1 + j], BTREE_KEY_SIZE);
        right.children[j] = node.children[mid + 1 + j];
    }
    right.children[right.nkeys] = node.children[node.nkeys];

    memcpy(promote_key, node.keys[mid], BTREE_KEY_SIZE);
    node.nkeys = mid;

    int new_page = pager_allocate_page(tree->pager);
    if (new_page < 0) return 0;
    node_store(tree->pager, page_num, &node);
    node_store(tree->pager, new_page, &right);
    *promote_page = new_page;
    *did_split = 1;
    return 1;
}

int btree_insert(BTree* tree, const Cell* key, int row_page, int row_offset) {
    if (tree == NULL || key == NULL) return 0;

    uint8_t enc[BTREE_KEY_SIZE];
    encode_key(key, enc);

    uint8_t promote_key[BTREE_KEY_SIZE];
    int32_t promote_page = 0;
    int did_split = 0;
    if (!insert_rec(tree, tree->root_page, enc, row_page, row_offset,
                    promote_key, &promote_page, &did_split)) {
        return 0;
    }
    if (!did_split) return 1;

    /* Root split: grow the tree one level. */
    int new_root = pager_allocate_page(tree->pager);
    if (new_root < 0) return 0;
    BtNode root;
    memset(&root, 0, sizeof(root));
    root.is_leaf = 0;
    root.nkeys = 1;
    memcpy(root.keys[0], promote_key, BTREE_KEY_SIZE);
    root.children[0] = tree->root_page;
    root.children[1] = promote_page;
    node_store(tree->pager, new_root, &root);
    tree->root_page = new_root;
    return 1;
}

/* -------------------------------------------------------------------------- */
/* Delete                                                                     */
/* -------------------------------------------------------------------------- */

static void leaf_remove_at(BtNode* node, int i) {
    for (int j = i + 1; j < node->nkeys; j++) {
        memcpy(node->keys[j - 1], node->keys[j], BTREE_KEY_SIZE);
        node->row_pages[j - 1] = node->row_pages[j];
        node->row_offsets[j - 1] = node->row_offsets[j];
    }
    node->nkeys--;
}

/* Drops separator sep and the child to its right from an internal node. */
static void internal_remove_separator(BtNode* node, int sep) {
    for (int j = sep + 1; j < node->nkeys; j++) {
        memcpy(node->keys[j - 1], node->keys[j], BTREE_KEY_SIZE);
    }
    for (int j = sep + 2; j <= node->nkeys; j++) {
        node->children[j - 1] = node->children[j];
    }
    node->nkeys--;
}

/* Moves the left sibling's last entry into child and refreshes the separator
   between them. */
static void borrow_from_left(BTree* tree, int parent_page, BtNode* parent, int idx,
                             BtNode* left, BtNode* child) {
    int left_page = parent->children[idx - 1];
    int child_page = parent->children[idx];

    if (child->is_leaf) {
        for (int j = child->nkeys; j > 0; j--) {
            memcpy(child->keys[j], child->keys[j - 1], BTREE_KEY_SIZE);
            child->row_pages[j] = child->row_pages[j - 1];
            child->row_offsets[j] = child->row_offsets[j - 1];
        }
        memcpy(child->keys[0], left->keys[left->nkeys - 1], BTREE_KEY_SIZE);
        child->row_pages[0] = left->row_pages[left->nkeys - 1];
        child->row_offsets[0] = left->row_offsets[left->nkeys - 1];
        child->nkeys++;
        left->nkeys--;
        /* Leaf separators are copies of the right child's first key. */
        memcpy(parent->keys[idx - 1], child->keys[0], BTREE_KEY_SIZE);
    } else {
        for (int j = child->nkeys; j > 0; j--) {
            memcpy(child->keys[j], child->keys[j - 1], BTREE_KEY_SIZE);
        }
        for (int j = child->nkeys + 1; j > 0; j--) {
            child->children[j] = child->children[j - 1];
        }
        /* The separator rotates down to bound the subtree that moves with it;
           the left sibling's last key becomes the new separator. */
        memcpy(child->keys[0], parent->keys[idx - 1], BTREE_KEY_SIZE);
        child->children[0] = left->children[left->nkeys];
        memcpy(parent->keys[idx - 1], left->keys[left->nkeys - 1], BTREE_KEY_SIZE);
        child->nkeys++;
        left->nkeys--;
    }
    node_store(tree->pager, left_page, left);
    node_store(tree->pager, child_page, child);
    node_store(tree->pager, parent_page, parent);
}

/* Mirror of borrow_from_left: takes the right sibling's first entry. */
static void borrow_from_right(BTree* tree, int parent_page, BtNode* parent, int idx,
                              BtNode* child, BtNode* right) {
    int child_page = parent->children[idx];
    int right_page = parent->children[idx + 1];

    if (child->is_leaf) {
        memcpy(child->keys[child->nkeys], right->keys[0], BTREE_KEY_SIZE);
        child->row_pages[child->nkeys] = right->row_pages[0];
        child->row_offsets[child->nkeys] = right->row_offsets[0];
        child->nkeys++;
        leaf_remove_at(right, 0);
        memcpy(parent->keys[idx], right->keys[0], BTREE_KEY_SIZE);
    } else {
        memcpy(child->keys[child->nkeys], parent->keys[idx], BTREE_KEY_SIZE);
        child->children[child->nkeys + 1] = right->children[0];
        child->nkeys++;
        memcpy(parent->keys[idx], right->keys[0], BTREE_KEY_SIZE);
        for (int j = 1; j < right->nkeys; j++) {
            memcpy(right->keys[j - 1], right->keys[j], BTREE_KEY_SIZE);
        }
        for (int j = 1; j <= right->nkeys; j++) {
            right->children[j - 1] = right->children[j];
        }
        right->nkeys--;
    }
    node_store(tree->pager, child_page, child);
    node_store(tree->pager, right_page, right);
    node_store(tree->pager, parent_page, parent);
}

/* Folds children sep and sep+1 into the left one and drops separator sep.
   Only ever called when neither sibling can spare an entry, so the result
   fits: leaves hold at most BTREE_LEAF_MIN + (BTREE_LEAF_MIN - 1) entries and
   internal nodes at most 2 * BTREE_INTERNAL_MIN keys. */
static void merge_children(BTree* tree, int parent_page, BtNode* parent, int sep) {
    int left_page = parent->children[sep];
    int right_page = parent->children[sep + 1];
    BtNode left;
    BtNode right;
    if (!node_load(tree->pager, left_page, &left)) return;
    if (!node_load(tree->pager, right_page, &right)) return;

    if (left.is_leaf) {
        for (int j = 0; j < right.nkeys; j++) {
            memcpy(left.keys[left.nkeys + j], right.keys[j], BTREE_KEY_SIZE);
            left.row_pages[left.nkeys + j] = right.row_pages[j];
            left.row_offsets[left.nkeys + j] = right.row_offsets[j];
        }
        left.nkeys += right.nkeys;
        left.next = right.next; /* keep the range-scan chain intact */
    } else {
        /* The separator is not a copy of anything below it, so it becomes a
           key of the merged node rather than being discarded. */
        memcpy(left.keys[left.nkeys], parent->keys[sep], BTREE_KEY_SIZE);
        left.nkeys++;
        for (int j = 0; j < right.nkeys; j++) {
            memcpy(left.keys[left.nkeys + j], right.keys[j], BTREE_KEY_SIZE);
        }
        for (int j = 0; j <= right.nkeys; j++) {
            left.children[left.nkeys + j] = right.children[j];
        }
        left.nkeys += right.nkeys;
    }

    internal_remove_separator(parent, sep);
    node_store(tree->pager, left_page, &left);
    node_store(tree->pager, parent_page, parent);
    pager_free_page(tree->pager, right_page);
}

/* Restores minimum occupancy for child idx: redistribute when a sibling has a
   spare entry, merge otherwise. Updates parent in place. */
static void rebalance_child(BTree* tree, int parent_page, BtNode* parent, int idx) {
    BtNode child;
    if (!node_load(tree->pager, parent->children[idx], &child)) return;
    int min = child.is_leaf ? BTREE_LEAF_MIN : BTREE_INTERNAL_MIN;
    if (child.nkeys >= min) return;

    if (idx > 0) {
        BtNode left;
        if (node_load(tree->pager, parent->children[idx - 1], &left) &&
            left.nkeys > min) {
            borrow_from_left(tree, parent_page, parent, idx, &left, &child);
            return;
        }
    }
    if (idx < parent->nkeys) {
        BtNode right;
        if (node_load(tree->pager, parent->children[idx + 1], &right) &&
            right.nkeys > min) {
            borrow_from_right(tree, parent_page, parent, idx, &child, &right);
            return;
        }
    }
    if (idx > 0) {
        merge_children(tree, parent_page, parent, idx - 1);
    } else if (parent->nkeys > 0) {
        merge_children(tree, parent_page, parent, idx);
    }
    /* A keyless internal node can only be the root, which btree_delete
       collapses; every other level is back at minimum occupancy here. */
}

/* Removes (key, locator) from the subtree at page_num. Sets *underflow when
   that subtree's root dropped below minimum occupancy and the caller must
   rebalance it. Returns 1 when an entry was removed. */
static int delete_rec(BTree* tree, int page_num, const uint8_t* key,
                      int row_page, int row_offset, int* underflow) {
    *underflow = 0;
    BtNode node;
    if (!node_load(tree->pager, page_num, &node)) return 0;

    if (node.is_leaf) {
        for (int i = 0; i < node.nkeys; i++) {
            int cmp = memcmp(node.keys[i], key, BTREE_KEY_SIZE);
            if (cmp > 0) break; /* past the key: this leaf does not hold it */
            if (cmp < 0) continue;
            if (node.row_pages[i] != row_page || node.row_offsets[i] != row_offset) {
                continue; /* same key, different row */
            }
            leaf_remove_at(&node, i);
            node_store(tree->pager, page_num, &node);
            *underflow = node.nkeys < BTREE_LEAF_MIN;
            return 1;
        }
        return 0;
    }

    /* Duplicates of one key can span several subtrees: descend into the
       leftmost candidate, then keep going right while the separator behind us
       still equals the key. */
    int i = 0;
    while (i < node.nkeys && memcmp(key, node.keys[i], BTREE_KEY_SIZE) > 0) {
        i++;
    }
    for (; i <= node.nkeys; i++) {
        if (i > 0 && memcmp(node.keys[i - 1], key, BTREE_KEY_SIZE) > 0) break;
        if (node.children[i] <= 0) break;
        int child_underflow = 0;
        if (!delete_rec(tree, node.children[i], key, row_page, row_offset,
                        &child_underflow)) {
            continue;
        }
        if (child_underflow) {
            rebalance_child(tree, page_num, &node, i);
        }
        *underflow = node.nkeys < BTREE_INTERNAL_MIN;
        return 1;
    }
    return 0;
}

int btree_delete(BTree* tree, const Cell* key, int row_page, int row_offset) {
    if (tree == NULL || key == NULL) return 0;

    uint8_t enc[BTREE_KEY_SIZE];
    encode_key(key, enc);

    /* The root is exempt from minimum occupancy, so its underflow flag is
       not actionable here. */
    int root_underflow = 0;
    if (!delete_rec(tree, tree->root_page, enc, row_page, row_offset,
                    &root_underflow)) {
        return 0;
    }

    /* Once merges leave the root without separators, its only child becomes
       the new root and the tree loses a level. */
    for (;;) {
        BtNode root;
        if (!node_load(tree->pager, tree->root_page, &root)) break;
        if (root.is_leaf || root.nkeys > 0) break;
        int child = root.children[0];
        if (child <= 0) break;
        pager_free_page(tree->pager, tree->root_page);
        tree->root_page = child;
    }
    return 1;
}

/* -------------------------------------------------------------------------- */
/* Scans                                                                      */
/* -------------------------------------------------------------------------- */

/* Returns the leaf page that could contain key, or 0 on corruption. */
static int find_leaf(BTree* tree, const uint8_t* key) {
    int page_num = tree->root_page;
    for (;;) {
        BtNode node;
        if (!node_load(tree->pager, page_num, &node)) return 0;
        if (node.is_leaf) return page_num;
        int i = 0;
        while (i < node.nkeys && memcmp(key, node.keys[i], BTREE_KEY_SIZE) > 0) {
            i++;
        }
        page_num = node.children[i];
        if (page_num <= 0) return 0;
    }
}

static int leftmost_leaf(BTree* tree) {
    int page_num = tree->root_page;
    for (;;) {
        BtNode node;
        if (!node_load(tree->pager, page_num, &node)) return 0;
        if (node.is_leaf) return page_num;
        page_num = node.children[0];
        if (page_num <= 0) return 0;
    }
}

int btree_scan_eq(BTree* tree, const Cell* key, BTreeScanFn fn, void* user) {
    if (tree == NULL || key == NULL || fn == NULL) return -1;

    uint8_t enc[BTREE_KEY_SIZE];
    encode_key(key, enc);

    int page_num = find_leaf(tree, enc);
    if (page_num == 0) return -1;

    int count = 0;
    int done = 0;
    while (page_num != 0 && !done) {
        BtNode node;
        if (!node_load(tree->pager, page_num, &node)) return -1;
        for (int i = 0; i < node.nkeys; i++) {
            int cmp = memcmp(node.keys[i], enc, BTREE_KEY_SIZE);
            if (cmp == 0) {
                fn(node.row_pages[i], node.row_offsets[i], user);
                count++;
            } else if (cmp > 0) {
                done = 1;
                break;
            }
        }
        page_num = node.next;
    }
    return count;
}

int btree_scan_range(BTree* tree, const Cell* lo, int lo_inclusive,
                     const Cell* hi, int hi_inclusive, BTreeScanFn fn, void* user) {
    if (tree == NULL || fn == NULL) return -1;

    uint8_t enc_lo[BTREE_KEY_SIZE];
    uint8_t enc_hi[BTREE_KEY_SIZE];
    if (lo != NULL) encode_key(lo, enc_lo);
    if (hi != NULL) encode_key(hi, enc_hi);

    int page_num = lo != NULL ? find_leaf(tree, enc_lo) : leftmost_leaf(tree);
    if (page_num == 0) return -1;

    int count = 0;
    int done = 0;
    while (page_num != 0 && !done) {
        BtNode node;
        if (!node_load(tree->pager, page_num, &node)) return -1;
        for (int i = 0; i < node.nkeys; i++) {
            if (lo != NULL) {
                int cmp = memcmp(node.keys[i], enc_lo, BTREE_KEY_SIZE);
                if (cmp < 0 || (cmp == 0 && !lo_inclusive)) continue;
            }
            if (hi != NULL) {
                int cmp = memcmp(node.keys[i], enc_hi, BTREE_KEY_SIZE);
                if (cmp > 0 || (cmp == 0 && !hi_inclusive)) {
                    done = 1;
                    break;
                }
            }
            fn(node.row_pages[i], node.row_offsets[i], user);
            count++;
        }
        page_num = node.next;
    }
    return count;
}
