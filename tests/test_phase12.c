#include "test_harness.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int run_auspex(const char* source, char* out, size_t out_size) {
    FILE* f = fopen("/tmp/test_phase12_src.apx", "w");
    if (f == NULL) return -1;
    fprintf(f, "%s", source);
    fclose(f);

    int rc = system("./bin/auspex /tmp/test_phase12_src.apx > /tmp/test_phase12_out.txt 2>&1");

    FILE* outf = fopen("/tmp/test_phase12_out.txt", "r");
    if (outf != NULL) {
        out[0] = '\0';
        size_t n = fread(out, 1, out_size - 1, outf);
        out[n] = '\0';
        fclose(outf);
    }
    return WEXITSTATUS(rc);
}

static int output_contains(const char* out, const char* substr) {
    return strstr(out, substr) != NULL;
}

static int count_occurrences(const char* out, const char* substr) {
    int count = 0;
    size_t len = strlen(substr);
    const char* p = out;
    while ((p = strstr(p, substr)) != NULL) {
        count++;
        p += len;
    }
    return count;
}

static void clean_trigger_db(void) {
    remove("auspex.db");
    remove("auspex.db.programs");
}

TEST(phase12_nextval_persists_across_restarts) {
    remove("auspex.db");
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    create_sequence(\"seq_a\", 100, 5);\n"
        "    print nextval(\"seq_a\");\n"
        "    print nextval(\"seq_a\");\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "100"));
    ASSERT_INT_EQ(1, output_contains(out, "105"));

    /* New process: the sequence continues where it left off. */
    rc = run_auspex(
        "proc main() -> int {\n"
        "    print nextval(\"seq_a\");\n"
        "    print currval(\"seq_a\");\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(2, count_occurrences(out, "110"));
}

TEST(phase12_currval_after_restart) {
    remove("auspex.db");
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    create_sequence(\"seq_b\", 7, 3);\n"
        "    print nextval(\"seq_b\");\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "7"));

    /* New process: currval works without calling nextval first. */
    rc = run_auspex(
        "proc main() -> int {\n"
        "    print currval(\"seq_b\");\n"
        "    print nextval(\"seq_b\");\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "7"));
    ASSERT_INT_EQ(1, output_contains(out, "10"));
}

TEST(phase12_drop_sequence_persists) {
    remove("auspex.db");
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    create_sequence(\"seq_c\", 1, 1);\n"
        "    print nextval(\"seq_c\");\n"
        "    drop_sequence(\"seq_c\");\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);

    /* New process: the dropped sequence stays gone. */
    rc = run_auspex(
        "proc main() -> int {\n"
        "    print nextval(\"seq_c\");\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(1, rc);
    ASSERT_INT_EQ(1, output_contains(out, "sequence does not exist"));
}

TEST(phase12_currval_before_nextval_errors) {
    remove("auspex.db");
    char out[512];
    /* currval before the first nextval is an error (has_value semantics). */
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    create_sequence(\"seq_d\", 50, 1);\n"
        "    print currval(\"seq_d\");\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(1, rc);

    /* The sequence exists after that run, but nextval was never called, so
       currval must still error in a new process. */
    rc = run_auspex(
        "proc main() -> int {\n"
        "    print currval(\"seq_d\");\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(1, rc);

    /* The first nextval in a new process still returns the start value. */
    rc = run_auspex(
        "proc main() -> int {\n"
        "    print nextval(\"seq_d\");\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "50"));
}

TEST(phase12_sequence_survives_other_catalog_writes) {
    remove("auspex.db");
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    create_sequence(\"seq_e\", 1000, 10);\n"
        "    print nextval(\"seq_e\");\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "1000"));

    /* Other DDL/DML rewrites the catalog page; the sequence must survive. */
    rc = run_auspex(
        "proc main() -> int {\n"
        "    create table t12 (id int, tag string);\n"
        "    insert into t12 values (1, \"x\");\n"
        "    print nextval(\"seq_e\");\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "1010"));

    rc = run_auspex(
        "proc main() -> int {\n"
        "    print nextval(\"seq_e\");\n"
        "    int n = -1;\n"
        "    select count(*) into n from t12;\n"
        "    print n;\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "1020"));
    ASSERT_INT_EQ(1, output_contains(out, "1"));
}

TEST(phase12_duplicate_create_after_restart_errors) {
    remove("auspex.db");
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    create_sequence(\"seq_f\", 1, 1);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);

    /* New process: re-creating a persisted sequence is a duplicate. */
    rc = run_auspex(
        "proc main() -> int {\n"
        "    create_sequence(\"seq_f\", 1, 1);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(1, rc);
}

TEST(phase12_missing_sequence_errors_unchanged) {
    remove("auspex.db");
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    print nextval(\"nope_seq\");\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(1, rc);
}

/* --- Row-level triggers (FOR EACH ROW, :new / :old) --- */

TEST(phase12_row_trigger_insert_fires_per_row) {
    clean_trigger_db();
    char out[512];
    int rc = run_auspex(
        "trigger trg_r_ins after insert on trg_ri for each row {\n"
        "    print \"row-ins\";\n"
        "    print :new.id;\n"
        "    print :new.name;\n"
        "}\n"
        "proc main() -> int {\n"
        "    create table trg_ri (id int, name string);\n"
        "    insert into trg_ri values (1, \"alice\");\n"
        "    insert into trg_ri values (2, \"bob\");\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(2, count_occurrences(out, "row-ins"));
    ASSERT_INT_EQ(1, count_occurrences(out, "alice"));
    ASSERT_INT_EQ(1, count_occurrences(out, "bob"));
    clean_trigger_db();
}

TEST(phase12_row_trigger_update_sees_old_and_new) {
    clean_trigger_db();
    char out[512];
    int rc = run_auspex(
        "trigger trg_r_upd after update on trg_ru for each row {\n"
        "    print \"upd\";\n"
        "    print :old.qty;\n"
        "    print :new.qty;\n"
        "}\n"
        "proc main() -> int {\n"
        "    create table trg_ru (id int, qty int);\n"
        "    insert into trg_ru values (1, 10);\n"
        "    insert into trg_ru values (2, 20);\n"
        "    insert into trg_ru values (3, 30);\n"
        "    update trg_ru set qty = 99 where id > 1;\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    /* two rows matched: (20 -> 99) and (30 -> 99) */
    ASSERT_INT_EQ(2, count_occurrences(out, "upd"));
    ASSERT_INT_EQ(1, count_occurrences(out, "20"));
    ASSERT_INT_EQ(1, count_occurrences(out, "30"));
    ASSERT_INT_EQ(2, count_occurrences(out, "99"));
    clean_trigger_db();
}

TEST(phase12_row_trigger_delete_sees_old) {
    clean_trigger_db();
    char out[512];
    int rc = run_auspex(
        "trigger trg_r_del after delete on trg_rd for each row {\n"
        "    print \"del\";\n"
        "    print :old.id;\n"
        "}\n"
        "proc main() -> int {\n"
        "    create table trg_rd (id int);\n"
        "    insert into trg_rd values (1);\n"
        "    insert into trg_rd values (2);\n"
        "    delete from trg_rd where id = 2;\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, count_occurrences(out, "del"));
    ASSERT_INT_EQ(1, count_occurrences(out, "2"));
    clean_trigger_db();
}

TEST(phase12_row_and_statement_triggers_both_fire) {
    clean_trigger_db();
    char out[512];
    int rc = run_auspex(
        "trigger trg_o_sb before insert on trg_ro {\n"
        "    print \"stmt-before\";\n"
        "}\n"
        "trigger trg_o_rb before insert on trg_ro for each row {\n"
        "    print \"row-before\";\n"
        "}\n"
        "trigger trg_o_ra after insert on trg_ro for each row {\n"
        "    print \"row-after\";\n"
        "}\n"
        "trigger trg_o_sa after insert on trg_ro {\n"
        "    print \"stmt-after\";\n"
        "}\n"
        "proc main() -> int {\n"
        "    create table trg_ro (id int);\n"
        "    insert into trg_ro values (1);\n"
        "    insert into trg_ro values (2);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    /* statement-level fires once per statement, row-level once per row */
    ASSERT_INT_EQ(2, count_occurrences(out, "stmt-before"));
    ASSERT_INT_EQ(2, count_occurrences(out, "stmt-after"));
    ASSERT_INT_EQ(2, count_occurrences(out, "row-before"));
    ASSERT_INT_EQ(2, count_occurrences(out, "row-after"));
    /* ordering within one statement:
       statement BEFORE -> row BEFORE -> write -> row AFTER -> statement AFTER */
    const char* sb = strstr(out, "stmt-before");
    const char* rb = strstr(out, "row-before");
    const char* ra = strstr(out, "row-after");
    const char* sa = strstr(out, "stmt-after");
    ASSERT(sb != NULL && rb != NULL && ra != NULL && sa != NULL);
    ASSERT(sb < rb && rb < ra && ra < sa);
    clean_trigger_db();
}

TEST(phase12_row_trigger_persists_across_restarts) {
    clean_trigger_db();
    char out[512];
    int rc = run_auspex(
        "trigger trg_r_per after insert on trg_rp for each row {\n"
        "    print \"row-persist-fired\";\n"
        "    print :new.id;\n"
        "}\n"
        "proc main() -> int {\n"
        "    create table trg_rp (id int);\n"
        "    insert into trg_rp values (1);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, count_occurrences(out, "row-persist-fired"));

    /* New process, source no longer declares the trigger: the persisted
       FOR EACH ROW definition is recompiled and keeps firing with :new. */
    rc = run_auspex(
        "proc main() -> int {\n"
        "    insert into trg_rp values (2);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, count_occurrences(out, "row-persist-fired"));
    ASSERT_INT_EQ(1, count_occurrences(out, "2"));
    clean_trigger_db();
}

TEST(phase12_row_trigger_fires_on_dynamic_sql) {
    clean_trigger_db();
    char out[512];
    int rc = run_auspex(
        "trigger trg_r_dyn after insert on trg_rdy for each row {\n"
        "    print \"dyn-row\";\n"
        "    print :new.id;\n"
        "}\n"
        "proc main() -> int {\n"
        "    create table trg_rdy (id int);\n"
        "    execute_immediate(\"insert into trg_rdy values (7)\");\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, count_occurrences(out, "dyn-row"));
    ASSERT_INT_EQ(1, count_occurrences(out, "7"));
    clean_trigger_db();
}

TEST(phase12_row_trigger_wrong_context_errors) {
    clean_trigger_db();
    char out[512];
    /* :new is not available in a DELETE trigger: runtime error. */
    int rc = run_auspex(
        "trigger trg_r_bad after delete on trg_rb for each row {\n"
        "    print :new.id;\n"
        "}\n"
        "proc main() -> int {\n"
        "    create table trg_rb (id int);\n"
        "    insert into trg_rb values (1);\n"
        "    delete from trg_rb where id = 1;\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(1, rc);
    ASSERT_INT_EQ(1, output_contains(out, "Cannot access field on non-row value"));
    clean_trigger_db();
}

TEST(phase12_row_trigger_requires_dml_event) {
    clean_trigger_db();
    char out[512];
    /* FOR EACH ROW only makes sense for row-changing events. */
    int rc = run_auspex(
        "trigger trg_r_ddl after create on trg_rddl for each row {\n"
        "    print \"nope\";\n"
        "}\n"
        "proc main() -> int {\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(1, rc);
    ASSERT_INT_EQ(1, output_contains(out, "'for each row' requires an insert, update, or delete trigger"));
    clean_trigger_db();
}

/* --- Trigger self-modification guard (#59) ---
   A trigger may not modify the table it fires on, directly or indirectly.
   Before the guard these overflowed the stack, crashed, or rewrote the row
   chain mid-trigger. */

#define SELF_MOD_ERROR "Cannot modify table"

TEST(phase12_trigger_static_dml_on_own_table_errors) {
    clean_trigger_db();
    char out[1024];
    int rc = run_auspex(
        "trigger trg_sm before insert on trg_sm_t {\n"
        "    insert into trg_sm_t values (99);\n"
        "}\n"
        "proc main() -> int {\n"
        "    create table trg_sm_t (id int);\n"
        "    insert into trg_sm_t values (1);\n"
        "    print \"unreachable\";\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(1, rc);
    ASSERT_INT_EQ(1, output_contains(out,
        "Cannot modify table 'trg_sm_t' while its trigger 'trg_sm' is running"));
    ASSERT_INT_EQ(0, output_contains(out, "Stack overflow"));
    ASSERT_INT_EQ(0, output_contains(out, "unreachable"));
    clean_trigger_db();
}

TEST(phase12_row_trigger_dynamic_dml_on_own_table_errors) {
    clean_trigger_db();
    char out[1024];
    int rc = run_auspex(
        "trigger trg_rd after insert on trg_rd_t for each row {\n"
        "    execute_immediate(\"insert into trg_rd_t values (99)\");\n"
        "}\n"
        "proc main() -> int {\n"
        "    create table trg_rd_t (id int);\n"
        "    insert into trg_rd_t values (1);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(1, rc);
    ASSERT_INT_EQ(1, output_contains(out,
        "Cannot modify table 'trg_rd_t' while its trigger 'trg_rd' is running"));
    clean_trigger_db();
}

TEST(phase12_trigger_dml_through_a_proc_errors) {
    /* No trigger fires for the UPDATE itself; the DML check catches it. */
    clean_trigger_db();
    char out[1024];
    int rc = run_auspex(
        "proc bump() -> int {\n"
        "    update trg_pr_t set id = 5;\n"
        "    return 0;\n"
        "}\n"
        "trigger trg_pr after insert on trg_pr_t {\n"
        "    bump();\n"
        "}\n"
        "proc main() -> int {\n"
        "    create table trg_pr_t (id int);\n"
        "    insert into trg_pr_t values (1);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(1, rc);
    ASSERT_INT_EQ(1, output_contains(out,
        "Cannot modify table 'trg_pr_t' while its trigger 'trg_pr' is running"));
    clean_trigger_db();
}

TEST(phase12_trigger_cycle_through_another_table_errors) {
    clean_trigger_db();
    char out[1024];
    int rc = run_auspex(
        "trigger trg_cy_a after insert on trg_cy_a_t {\n"
        "    insert into trg_cy_b_t values (1);\n"
        "}\n"
        "trigger trg_cy_b after insert on trg_cy_b_t {\n"
        "    insert into trg_cy_a_t values (2);\n"
        "}\n"
        "proc main() -> int {\n"
        "    create table trg_cy_a_t (id int);\n"
        "    create table trg_cy_b_t (id int);\n"
        "    insert into trg_cy_a_t values (1);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(1, rc);
    ASSERT_INT_EQ(1, output_contains(out,
        "Cannot modify table 'trg_cy_a_t' while its trigger 'trg_cy_a' is running"));
    ASSERT_INT_EQ(0, output_contains(out, "Stack overflow"));
    clean_trigger_db();
}

TEST(phase12_trigger_self_modification_error_is_catchable) {
    clean_trigger_db();
    char out[1024];
    int rc = run_auspex(
        "trigger trg_ca after insert on trg_ca_t {\n"
        "    delete from trg_ca_t;\n"
        "}\n"
        "proc main() -> int {\n"
        "    create table trg_ca_t (id int);\n"
        "    try {\n"
        "        insert into trg_ca_t values (1);\n"
        "    } catch (err) {\n"
        "        print concat(\"caught: \", err);\n"
        "    }\n"
        "    print \"continues\";\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "caught: "));
    ASSERT_INT_EQ(1, output_contains(out, SELF_MOD_ERROR));
    ASSERT_INT_EQ(1, output_contains(out, "continues"));
    clean_trigger_db();
}

/* --- dbms_sql cursor API (Phase 12 Task 4) --- */

static void clean_dbms_sql_db(void) {
    remove("auspex.db");
    remove("auspex.db.programs");
}

TEST(phase12_dbms_sql_open_parse_execute_dml) {
    clean_dbms_sql_db();
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    create table dbms_t (id int, name string);\n"
        "    int c = dbms_sql.open_cursor();\n"
        "    dbms_sql.parse(c, \"insert into dbms_t values (1, 'alice')\");\n"
        "    int n = dbms_sql.execute_cursor(c);\n"
        "    print n;\n"
        "    dbms_sql.close_cursor(c);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "1"));
    clean_dbms_sql_db();
}

TEST(phase12_dbms_sql_fetch_rows) {
    clean_dbms_sql_db();
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    create table dbms_t (id int, name string);\n"
        "    insert into dbms_t values (1, 'alice');\n"
        "    insert into dbms_t values (2, 'bob');\n"
        "    int c = dbms_sql.open_cursor();\n"
        "    dbms_sql.parse(c, \"select id, name from dbms_t order by id\");\n"
        "    dbms_sql.execute_cursor(c);\n"
        "    array<row> rows = dbms_sql.fetch_rows(c, 10);\n"
        "    print length(rows);\n"
        "    print rows[0].id;\n"
        "    print rows[0].name;\n"
        "    print rows[1].id;\n"
        "    print rows[1].name;\n"
        "    dbms_sql.close_cursor(c);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "2"));
    ASSERT_INT_EQ(1, output_contains(out, "1"));
    ASSERT_INT_EQ(1, output_contains(out, "alice"));
    ASSERT_INT_EQ(1, output_contains(out, "2"));
    ASSERT_INT_EQ(1, output_contains(out, "bob"));
    clean_dbms_sql_db();
}

TEST(phase12_dbms_sql_column_value) {
    clean_dbms_sql_db();
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    create table dbms_t (id int, name string);\n"
        "    insert into dbms_t values (7, 'carol');\n"
        "    int c = dbms_sql.open_cursor();\n"
        "    dbms_sql.parse(c, \"select id, name from dbms_t\");\n"
        "    dbms_sql.execute_cursor(c);\n"
        "    array<row> rows = dbms_sql.fetch_rows(c, 1);\n"
        "    any id = dbms_sql.column_value(c, 0);\n"
        "    any name = dbms_sql.column_value(c, 1);\n"
        "    print id;\n"
        "    print name;\n"
        "    dbms_sql.close_cursor(c);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "7"));
    ASSERT_INT_EQ(1, output_contains(out, "carol"));
    clean_dbms_sql_db();
}

TEST(phase12_dbms_sql_close_invalidates_handle) {
    clean_dbms_sql_db();
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    int c = dbms_sql.open_cursor();\n"
        "    dbms_sql.close_cursor(c);\n"
        "    dbms_sql.execute_cursor(c);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(1, rc);
    ASSERT_INT_EQ(1, output_contains(out, "invalid cursor handle"));
    clean_dbms_sql_db();
}

static int build_marshal_shared_lib(void) {
    FILE* f = fopen("/tmp/test_phase12_ext.c", "w");
    if (f == NULL) return 0;
    fprintf(f,
        "#include <string.h>\n"
        "#include <stdio.h>\n"
        "double ext_half(int x) { return x / 2.0; }\n"
        "double ext_scale(double x) { return x * 2.5; }\n"
        "int ext_floor(double x) { return (int)x; }\n"
        "int ext_len(const char* s) { return (int)strlen(s); }\n"
        "const char* ext_greet(const char* s) {\n"
        "    static char buf[64];\n"
        "    snprintf(buf, sizeof(buf), \"hello, %%s\", s);\n"
        "    return buf;\n"
        "}\n"
        "const char* ext_label(int n) { return n > 0 ? \"positive\" : \"non-positive\"; }\n"
        "const char* ext_float_label(double x) { return x < 0 ? \"neg\" : \"nonneg\"; }\n"
        "const char* ext_null(const char* s) { (void)s; return NULL; }\n");
    fclose(f);
    int rc = system("cc -shared -fPIC -o /tmp/test_phase12_ext.so /tmp/test_phase12_ext.c");
    return rc == 0;
}

TEST(phase12_external_call_float_return) {
    ASSERT_INT_EQ(1, build_marshal_shared_lib());
    char out[256];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    float h = external_call_float(\"/tmp/test_phase12_ext.so\", \"ext_half\", 7);\n"
        "    float s = external_call_float(\"/tmp/test_phase12_ext.so\", \"ext_scale\", 1.5);\n"
        "    print float_to_string(h);\n"
        "    print float_to_string(s);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "3.5"));
    ASSERT_INT_EQ(1, output_contains(out, "3.75"));
}

TEST(phase12_external_call_int_return_from_float_and_string) {
    ASSERT_INT_EQ(1, build_marshal_shared_lib());
    char out[256];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    int f = external_call(\"/tmp/test_phase12_ext.so\", \"ext_floor\", 9.75);\n"
        "    int n = external_call(\"/tmp/test_phase12_ext.so\", \"ext_len\", \"marshal\");\n"
        "    print int_to_string(f);\n"
        "    print int_to_string(n);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "9\n7\n"));
}

TEST(phase12_external_call_string_return) {
    ASSERT_INT_EQ(1, build_marshal_shared_lib());
    char out[256];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    string g = external_call_string(\"/tmp/test_phase12_ext.so\", \"ext_greet\", \"auspex\");\n"
        "    string a = external_call_string(\"/tmp/test_phase12_ext.so\", \"ext_label\", 3);\n"
        "    string b = external_call_string(\"/tmp/test_phase12_ext.so\", \"ext_float_label\", -0.5);\n"
        "    print g;\n"
        "    print a;\n"
        "    print b;\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "hello, auspex\npositive\nneg\n"));
}

TEST(phase12_external_call_string_result_is_copied) {
    ASSERT_INT_EQ(1, build_marshal_shared_lib());
    char out[256];
    /* ext_greet reuses one static buffer; the first result must not change. */
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    string first = external_call_string(\"/tmp/test_phase12_ext.so\", \"ext_greet\", \"one\");\n"
        "    string second = external_call_string(\"/tmp/test_phase12_ext.so\", \"ext_greet\", \"two\");\n"
        "    print first;\n"
        "    print second;\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "hello, one\nhello, two\n"));
}

TEST(phase12_external_call_string_null_return_is_null) {
    ASSERT_INT_EQ(1, build_marshal_shared_lib());
    char out[256];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    print nvl(external_call_string(\"/tmp/test_phase12_ext.so\", \"ext_null\", \"x\"), \"fallback\");\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "fallback"));
}

TEST(phase12_external_call_rejects_bool_argument) {
    char out[256];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    external_call_float(\"/tmp/test_phase12_ext.so\", \"ext_scale\", true);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(1, rc);
    ASSERT_INT_EQ(1, output_contains(out, "external_call_float expects an int, float or string argument"));
}

TEST(phase12_external_call_string_result_type_is_checked) {
    char out[256];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    int n = external_call_string(\"/tmp/test_phase12_ext.so\", \"ext_label\", 1);\n"
        "    return n;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(1, rc);
}

TEST(phase12_external_call_string_missing_symbol_fails) {
    ASSERT_INT_EQ(1, build_marshal_shared_lib());
    char out[256];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    external_call_string(\"/tmp/test_phase12_ext.so\", \"no_such_symbol\", \"x\");\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(1, rc);
    ASSERT_INT_EQ(1, output_contains(out, "external_call_string:"));
}

/* --- external_call_sig (#61) ---
   Every descriptor from "i()" to "s(ssss)" goes through its own function
   pointer prototype, spelled out by X-macros in natives.c. This sweep builds
   a library with one function per descriptor and calls all 363 from Auspex:
   a prototype with a wrong type would scramble or crash its call. */

static const char* const SIG_CTYPES[] = {"int", "double", "const char*"};

static int sig_pow3(int n) {
    int p = 1;
    for (int i = 0; i < n; i++) p *= 3;
    return p;
}

/* Argument i of a sweep call is i + 2 as an int, i + 2.5 as a double, or a
   string of i + 2 characters; each function returns twice the sum of
   (i + 1) * argument (string length for strings), which is a whole number. */
static int sig_expected(const int* types, int n) {
    double v = 0;
    for (int i = 0; i < n; i++) {
        double arg = types[i] == 1 ? i + 2.5 : i + 2;
        v += (i + 1) * arg;
    }
    return (int)(v * 2);
}

static void sig_name(char* buf, size_t size, int ret, const int* types, int n) {
    int len = snprintf(buf, size, "sig_%c_", "ids"[ret]);
    for (int i = 0; i < n; i++) buf[len++] = "ids"[types[i]];
    buf[len] = '\0';
}

TEST(phase12_external_call_sig_sweeps_every_signature) {
    FILE* c = fopen("/tmp/test_phase12_sig.c", "w");
    ASSERT_PTR_NOT_NULL(c);
    fprintf(c, "#include <stdio.h>\n#include <string.h>\n");
    size_t src_cap = 1 << 17;
    char* src = malloc(src_cap);
    ASSERT_PTR_NOT_NULL(src);
    size_t src_len = (size_t)snprintf(src, src_cap, "proc main() -> int {\n");
    int calls = 0;
    for (int ret = 0; ret < 3; ret++) {
        for (int n = 0; n <= 4; n++) {
            for (int code = 0; code < sig_pow3(n); code++) {
                int types[4];
                for (int i = 0; i < n; i++) types[i] = (code / sig_pow3(i)) % 3;
                char name[32];
                sig_name(name, sizeof(name), ret, types, n);

                /* C side */
                fprintf(c, "%s %s(", SIG_CTYPES[ret], name);
                if (n == 0) fprintf(c, "void");
                for (int i = 0; i < n; i++) {
                    fprintf(c, "%s%s a%d", i > 0 ? ", " : "", SIG_CTYPES[types[i]], i);
                }
                fprintf(c, ") {\n    double v = 0;\n");
                for (int i = 0; i < n; i++) {
                    fprintf(c, types[i] == 2 ? "    v += %d * (double)strlen(a%d);\n"
                                             : "    v += %d * (double)a%d;\n", i + 1, i);
                }
                fprintf(c, "    int k = (int)(v * 2);\n");
                if (ret == 2) {
                    fprintf(c, "    static char buf[16];\n"
                               "    snprintf(buf, sizeof(buf), \"%%d\", k);\n"
                               "    return buf;\n}\n");
                } else {
                    fprintf(c, "    return k;\n}\n");
                }

                /* Auspex side: print "name:result" */
                char sig[16];
                int sl = snprintf(sig, sizeof(sig), "%c(", "ids"[ret]);
                for (int i = 0; i < n; i++) sig[sl++] = "ids"[types[i]];
                sig[sl++] = ')';
                sig[sl] = '\0';
                char args[128];
                size_t al = 0;
                args[0] = '\0';
                for (int i = 0; i < n; i++) {
                    if (types[i] == 0) {
                        al += (size_t)snprintf(args + al, sizeof(args) - al, ", %d", i + 2);
                    } else if (types[i] == 1) {
                        al += (size_t)snprintf(args + al, sizeof(args) - al, ", %d.5", i + 2);
                    } else {
                        al += (size_t)snprintf(args + al, sizeof(args) - al, ", \"%.*s\"",
                                               i + 2, "xxxxxxxx");
                    }
                }
                const char* wrap_open = ret == 0 ? "int_to_string(" : ret == 1 ? "float_to_string(" : "";
                const char* wrap_close = ret == 2 ? "" : ")";
                src_len += (size_t)snprintf(src + src_len, src_cap - src_len,
                    "    print concat(\"%s:\", %sexternal_call_sig(\"/tmp/test_phase12_sig.so\", "
                    "\"%s\", \"%s\"%s)%s);\n",
                    name, wrap_open, name, sig, args, wrap_close);
                calls++;
            }
        }
    }
    snprintf(src + src_len, src_cap - src_len, "    return 0;\n}\n");
    fclose(c);
    ASSERT_INT_EQ(363, calls);
    ASSERT_INT_EQ(0, system("cc -shared -fPIC -o /tmp/test_phase12_sig.so /tmp/test_phase12_sig.c"));

    char* out = malloc(1 << 15);
    ASSERT_PTR_NOT_NULL(out);
    int rc = run_auspex(src, out, 1 << 15);
    free(src);
    if (rc != 0) fprintf(stderr, "%.400s\n", out);
    ASSERT_INT_EQ(0, rc);

    int matched = 0;
    for (int ret = 0; ret < 3; ret++) {
        for (int n = 0; n <= 4; n++) {
            for (int code = 0; code < sig_pow3(n); code++) {
                int types[4];
                for (int i = 0; i < n; i++) types[i] = (code / sig_pow3(i)) % 3;
                char line[64];
                char name[32];
                sig_name(name, sizeof(name), ret, types, n);
                snprintf(line, sizeof(line), "%s:%d\n", name, sig_expected(types, n));
                if (strstr(out, line) != NULL) {
                    matched++;
                } else {
                    fprintf(stderr, "    missing %s", line);
                }
            }
        }
    }
    free(out);
    ASSERT_INT_EQ(363, matched);
}

TEST(phase12_external_call_sig_checks_signature_at_compile_time) {
    char out[512];
    struct {
        const char* call;
        const char* error;
    } cases[] = {
        {"external_call_sig(\"libm.so.6\", \"pow\", \"d(dd)\", 2.0)",
         "signature 'd(dd)' takes 2 argument(s), got 1"},
        {"external_call_sig(\"libm.so.6\", \"pow\", \"d(dd)\", 2.0, \"ten\")",
         "argument 2 must be a float, as signature 'd(dd)' says"},
        {"external_call_sig(\"libm.so.6\", \"pow\", \"q(dd)\", 2.0, 1.0)",
         "must look like"},
        {"external_call_sig(\"libm.so.6\", \"pow\", \"d(ddddd)\", 1.0, 1.0, 1.0, 1.0, 1.0)",
         "at most 4 arguments"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char src[512];
        snprintf(src, sizeof(src),
                 "proc main() -> int {\n    print %s;\n    return 0;\n}\n", cases[i].call);
        int rc = run_auspex(src, out, sizeof(out));
        ASSERT_INT_EQ(1, rc);
        ASSERT_INT_EQ(1, output_contains(out, "Compile error"));
        ASSERT_INT_EQ(1, output_contains(out, cases[i].error));
    }
    /* A signature that is not a literal cannot be checked, so it is refused. */
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    string sig = \"d(dd)\";\n"
        "    print external_call_sig(\"libm.so.6\", \"pow\", sig, 2.0, 3.0);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(1, rc);
    ASSERT_INT_EQ(1, output_contains(out, "needs its signature as a string literal"));
}

TEST(phase12_external_call_sig_types_its_result) {
    /* A library of its own: libm/libc sonames differ between Linux and macOS. */
    FILE* f = fopen("/tmp/test_phase12_sig_typed.c", "w");
    ASSERT_PTR_NOT_NULL(f);
    fprintf(f,
        "#include <string.h>\n"
        "double scale(double x, int shift) { return x * (1 << shift); }\n"
        "int compare(const char* a, const char* b) { return strcmp(a, b) == 0; }\n"
        "const char* pick(int which, const char* a, const char* b) {\n"
        "    return which ? a : b;\n"
        "}\n");
    fclose(f);
    ASSERT_INT_EQ(0, system("cc -shared -fPIC -o /tmp/test_phase12_sig_typed.so "
                            "/tmp/test_phase12_sig_typed.c"));
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    string lib = \"/tmp/test_phase12_sig_typed.so\";\n"
        "    float x = external_call_sig(lib, \"scale\", \"d(di)\", 1.5, 3);\n"
        "    int same = external_call_sig(lib, \"compare\", \"i(ss)\", \"abc\", \"abc\");\n"
        "    string p = external_call_sig(lib, \"pick\", \"s(iss)\", 0, \"first\", \"second\");\n"
        "    float y = external_call_sig(lib, \"scale\", \"d(di)\", 2, 1);\n"
        "    print concat(\"scale:\", float_to_string(x));\n"
        "    print concat(\"same:\", int_to_string(same));\n"
        "    print concat(\"pick:\", p);\n"
        "    print concat(\"int-as-float:\", float_to_string(y));\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "scale:12"));
    ASSERT_INT_EQ(1, output_contains(out, "same:1"));
    ASSERT_INT_EQ(1, output_contains(out, "pick:second"));
    ASSERT_INT_EQ(1, output_contains(out, "int-as-float:4"));
}

#ifdef USE_SQLITE
static int run_auspex_sqlite(const char* source, char* out, size_t out_size) {
    FILE* f = fopen("/tmp/test_phase12_sql_src.apx", "w");
    if (f == NULL) return -1;
    fprintf(f, "%s", source);
    fclose(f);

    int rc = system("./bin/auspex /tmp/test_phase12_sql_src.apx --db /tmp/test_phase12.db"
                    " > /tmp/test_phase12_sql_out.txt 2>&1");

    FILE* outf = fopen("/tmp/test_phase12_sql_out.txt", "r");
    if (outf != NULL) {
        out[0] = '\0';
        size_t n = fread(out, 1, out_size - 1, outf);
        out[n] = '\0';
        fclose(outf);
    }
    return WEXITSTATUS(rc);
}

TEST(phase12_trigger_static_fires) {
    clean_trigger_db();
    char out[512];
    int rc = run_auspex(
        "trigger trg_static after insert on trg_s {\n"
        "    print \"static-fired\";\n"
        "}\n"
        "proc main() -> int {\n"
        "    create table trg_s (id int);\n"
        "    insert into trg_s values (1);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, count_occurrences(out, "static-fired"));
    clean_trigger_db();
}

TEST(phase12_trigger_persists_across_restarts) {
    clean_trigger_db();
    char out[512];
    int rc = run_auspex(
        "trigger trg_persist after insert on trg_p {\n"
        "    print \"persist-fired\";\n"
        "}\n"
        "proc main() -> int {\n"
        "    create table trg_p (id int);\n"
        "    insert into trg_p values (1);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, count_occurrences(out, "persist-fired"));

    /* New process, source no longer declares the trigger: it is reloaded
       from the persisted program units and still fires. */
    rc = run_auspex(
        "proc main() -> int {\n"
        "    insert into trg_p values (2);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, count_occurrences(out, "persist-fired"));
    clean_trigger_db();
}

TEST(phase12_drop_trigger_stops_and_persists) {
    clean_trigger_db();
    char out[512];
    int rc = run_auspex(
        "trigger trg_drop after insert on trg_d {\n"
        "    print \"drop-fired\";\n"
        "}\n"
        "proc main() -> int {\n"
        "    create table trg_d (id int);\n"
        "    insert into trg_d values (1);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, count_occurrences(out, "drop-fired"));

    /* DROP TRIGGER stops firing immediately ... */
    rc = run_auspex(
        "proc main() -> int {\n"
        "    drop trigger trg_drop;\n"
        "    insert into trg_d values (2);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(0, count_occurrences(out, "drop-fired"));

    /* ... and the trigger stays dropped after a restart. */
    rc = run_auspex(
        "proc main() -> int {\n"
        "    insert into trg_d values (3);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(0, count_occurrences(out, "drop-fired"));
    clean_trigger_db();
}

/* The SQLite driver fires row triggers through its own snapshot path. */
TEST(phase12_sqlite_row_trigger_dml_on_own_table_errors) {
    remove("/tmp/test_phase12.db");
    char out[1024];
    int rc = run_auspex_sqlite(
        "trigger trg_sq after insert on trg_sq_t for each row {\n"
        "    execute_immediate(\"delete from trg_sq_t\");\n"
        "}\n"
        "proc main() -> int {\n"
        "    create table trg_sq_t (id int);\n"
        "    insert into trg_sq_t values (1);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(1, rc);
    ASSERT_INT_EQ(1, output_contains(out,
        "Cannot modify table 'trg_sq_t' while its trigger 'trg_sq' is running"));
    remove("/tmp/test_phase12.db");
}

/* A row trigger's own error reaches the statement's error message on SQLite,
   as it does on the custom engine, instead of a bare "SQL execution failed". */
TEST(phase12_sqlite_row_trigger_error_message_is_reported) {
    remove("/tmp/test_phase12.db");
    char out[1024];
    int rc = run_auspex_sqlite(
        "trigger trg_sqe before insert on trg_sqe_t for each row {\n"
        "    raise_application_error(-20001, \"negative total\");\n"
        "}\n"
        "proc main() -> int {\n"
        "    create table trg_sqe_t (id int);\n"
        "    insert into trg_sqe_t values (1);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(1, rc);
    ASSERT_INT_EQ(1, output_contains(out, "negative total"));
    remove("/tmp/test_phase12.db");
}

TEST(phase12_trigger_fires_on_execute_immediate) {
    clean_trigger_db();
    char out[512];
    int rc = run_auspex(
        "trigger trg_dyn_pre before insert on trg_dyn {\n"
        "    print \"dyn-before\";\n"
        "}\n"
        "trigger trg_dyn_post after insert on trg_dyn {\n"
        "    print \"dyn-after\";\n"
        "}\n"
        "proc main() -> int {\n"
        "    create table trg_dyn (id int);\n"
        "    execute_immediate(\"insert into trg_dyn values (1)\");\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, count_occurrences(out, "dyn-before"));
    ASSERT_INT_EQ(1, count_occurrences(out, "dyn-after"));
    /* before must fire before after */
    const char* b = strstr(out, "dyn-before");
    const char* a = strstr(out, "dyn-after");
    ASSERT(b != NULL && a != NULL && b < a);
    clean_trigger_db();
}

TEST(phase12_trigger_fires_on_dbms_sql_execute) {
    clean_trigger_db();
    char out[512];
    int rc = run_auspex(
        "trigger trg_dsql after insert on trg_ds {\n"
        "    print \"dbmssql-fired\";\n"
        "}\n"
        "proc main() -> int {\n"
        "    create table trg_ds (id int);\n"
        "    dbms_sql.execute(\"insert into trg_ds values (1)\");\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, count_occurrences(out, "dbmssql-fired"));
    clean_trigger_db();
}

TEST(phase12_drop_trigger_via_execute_immediate) {
    clean_trigger_db();
    char out[512];
    int rc = run_auspex(
        "trigger trg_dyndrop after insert on trg_dd {\n"
        "    print \"dyndrop-fired\";\n"
        "}\n"
        "proc main() -> int {\n"
        "    create table trg_dd (id int);\n"
        "    execute_immediate(\"insert into trg_dd values (1)\");\n"
        "    execute_immediate(\"drop trigger trg_dyndrop\");\n"
        "    execute_immediate(\"insert into trg_dd values (2)\");\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    /* fires for the first insert only */
    ASSERT_INT_EQ(1, count_occurrences(out, "dyndrop-fired"));

    /* The dynamic drop persists: no firing in a new process. */
    rc = run_auspex(
        "proc main() -> int {\n"
        "    insert into trg_dd values (3);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(0, count_occurrences(out, "dyndrop-fired"));
    clean_trigger_db();
}

TEST(phase12_sqlite_trigger_persists_across_restarts) {
    remove("/tmp/test_phase12_trg.db");
    char out[512];
    int rc = run_auspex_sqlite(
        "trigger strg_persist after insert on strg_p {\n"
        "    print \"sqlite-persist-fired\";\n"
        "}\n"
        "proc main() -> int {\n"
        "    create table strg_p (id int);\n"
        "    insert into strg_p values (1);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, count_occurrences(out, "sqlite-persist-fired"));

    rc = run_auspex_sqlite(
        "proc main() -> int {\n"
        "    insert into strg_p values (2);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, count_occurrences(out, "sqlite-persist-fired"));
    remove("/tmp/test_phase12_trg.db");
}

TEST(phase12_sqlite_drop_trigger_persists) {
    remove("/tmp/test_phase12_trg.db");
    char out[512];
    int rc = run_auspex_sqlite(
        "trigger strg_drop after insert on strg_d {\n"
        "    print \"sqlite-drop-fired\";\n"
        "}\n"
        "proc main() -> int {\n"
        "    create table strg_d (id int);\n"
        "    insert into strg_d values (1);\n"
        "    drop trigger strg_drop;\n"
        "    insert into strg_d values (2);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, count_occurrences(out, "sqlite-drop-fired"));

    rc = run_auspex_sqlite(
        "proc main() -> int {\n"
        "    insert into strg_d values (3);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(0, count_occurrences(out, "sqlite-drop-fired"));
    remove("/tmp/test_phase12_trg.db");
}

TEST(phase12_sqlite_nextval_persists_across_restarts) {
    remove("/tmp/test_phase12.db");
    char out[512];
    int rc = run_auspex_sqlite(
        "proc main() -> int {\n"
        "    create_sequence(\"sseq\", 200, 25);\n"
        "    print nextval(\"sseq\");\n"
        "    print nextval(\"sseq\");\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "200"));
    ASSERT_INT_EQ(1, output_contains(out, "225"));

    /* New process against the same SQLite file: currval and nextval
       continue from the persisted state. */
    rc = run_auspex_sqlite(
        "proc main() -> int {\n"
        "    print currval(\"sseq\");\n"
        "    print nextval(\"sseq\");\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "225"));
    ASSERT_INT_EQ(1, output_contains(out, "250"));
}

TEST(phase12_sqlite_drop_sequence_persists) {
    remove("/tmp/test_phase12.db");
    char out[512];
    int rc = run_auspex_sqlite(
        "proc main() -> int {\n"
        "    create_sequence(\"sseq2\", 1, 1);\n"
        "    print nextval(\"sseq2\");\n"
        "    drop_sequence(\"sseq2\");\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);

    rc = run_auspex_sqlite(
        "proc main() -> int {\n"
        "    print nextval(\"sseq2\");\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(1, rc);
    ASSERT_INT_EQ(1, output_contains(out, "sequence does not exist"));
}

TEST(phase12_sqlite_row_trigger_insert_fires_per_row) {
    remove("/tmp/test_phase12.db");
    char out[512];
    int rc = run_auspex_sqlite(
        "trigger strg_r_ins after insert on strg_ri for each row {\n"
        "    print \"s-row-ins\";\n"
        "    print :new.id;\n"
        "    print :new.name;\n"
        "}\n"
        "proc main() -> int {\n"
        "    create table strg_ri (id int, name string);\n"
        "    insert into strg_ri values (1, \"alice\");\n"
        "    insert into strg_ri values (2, \"bob\");\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(2, count_occurrences(out, "s-row-ins"));
    ASSERT_INT_EQ(1, count_occurrences(out, "alice"));
    ASSERT_INT_EQ(1, count_occurrences(out, "bob"));
    remove("/tmp/test_phase12.db");
}

TEST(phase12_sqlite_row_trigger_update_and_delete) {
    remove("/tmp/test_phase12.db");
    char out[512];
    int rc = run_auspex_sqlite(
        "trigger strg_r_upd after update on strg_ru for each row {\n"
        "    print \"s-upd\";\n"
        "    print :old.qty;\n"
        "    print :new.qty;\n"
        "}\n"
        "trigger strg_r_del after delete on strg_ru for each row {\n"
        "    print \"s-del\";\n"
        "    print :old.id;\n"
        "}\n"
        "proc main() -> int {\n"
        "    create table strg_ru (id int, qty int);\n"
        "    insert into strg_ru values (1, 10);\n"
        "    insert into strg_ru values (2, 20);\n"
        "    update strg_ru set qty = 99 where id = 2;\n"
        "    delete from strg_ru where id = 1;\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, count_occurrences(out, "s-upd"));
    ASSERT_INT_EQ(1, count_occurrences(out, "s-del"));
    /* update: 20 -> 99; delete: id 1 */
    ASSERT_INT_EQ(1, count_occurrences(out, "20"));
    ASSERT_INT_EQ(1, count_occurrences(out, "99"));
    remove("/tmp/test_phase12.db");
}

TEST(phase12_sqlite_row_trigger_persists_across_restarts) {
    remove("/tmp/test_phase12.db");
    char out[512];
    int rc = run_auspex_sqlite(
        "trigger strg_r_per after insert on strg_rp for each row {\n"
        "    print \"s-row-persist-fired\";\n"
        "    print :new.id;\n"
        "}\n"
        "proc main() -> int {\n"
        "    create table strg_rp (id int);\n"
        "    insert into strg_rp values (1);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, count_occurrences(out, "s-row-persist-fired"));

    rc = run_auspex_sqlite(
        "proc main() -> int {\n"
        "    insert into strg_rp values (2);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, count_occurrences(out, "s-row-persist-fired"));
    ASSERT_INT_EQ(1, count_occurrences(out, "2"));
    remove("/tmp/test_phase12.db");
}

TEST(phase12_dbms_sql_sqlite_bind_and_execute) {
    remove("/tmp/test_phase12.db");
    char out[512];
    int rc = run_auspex_sqlite(
        "proc main() -> int {\n"
        "    create table dbms_t (id int, name string);\n"
        "    int c = dbms_sql.open_cursor();\n"
        "    dbms_sql.parse(c, \"insert into dbms_t values (?1, ?2)\");\n"
        "    dbms_sql.bind_variable(c, \"1\", 42);\n"
        "    dbms_sql.bind_variable(c, \"2\", \"dave\");\n"
        "    int n = dbms_sql.execute_cursor(c);\n"
        "    print n;\n"
        "    dbms_sql.parse(c, \"select id, name from dbms_t where id = ?1\");\n"
        "    dbms_sql.bind_variable(c, \"1\", 42);\n"
        "    dbms_sql.execute_cursor(c);\n"
        "    array<row> rows = dbms_sql.fetch_rows(c, 10);\n"
        "    print length(rows);\n"
        "    print rows[0].id;\n"
        "    print rows[0].name;\n"
        "    dbms_sql.close_cursor(c);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "1"));
    ASSERT_INT_EQ(1, output_contains(out, "42"));
    ASSERT_INT_EQ(1, output_contains(out, "dave"));
    remove("/tmp/test_phase12.db");
}

TEST(phase12_dbms_sql_sqlite_bind_string_with_quote) {
    remove("/tmp/test_phase12.db");
    char out[512];
    int rc = run_auspex_sqlite(
        "proc main() -> int {\n"
        "    create table dbms_t (id int, name string);\n"
        "    int c = dbms_sql.open_cursor();\n"
        "    dbms_sql.parse(c, \"insert into dbms_t values (?1, ?2)\");\n"
        "    dbms_sql.bind_variable(c, \"1\", 1);\n"
        "    dbms_sql.bind_variable(c, \"2\", \"o'brien\");\n"
        "    dbms_sql.execute_cursor(c);\n"
        "    dbms_sql.parse(c, \"select name from dbms_t\");\n"
        "    dbms_sql.execute_cursor(c);\n"
        "    array<row> rows = dbms_sql.fetch_rows(c, 1);\n"
        "    print rows[0].name;\n"
        "    dbms_sql.close_cursor(c);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "o'brien"));
    remove("/tmp/test_phase12.db");
}
#endif

/* ===== Issue #28: user packages can override built-in packages =====
 *
 * Built-in packages (dbms_output, utl_file, dbms_sql) are prepended to the
 * user source on every compilation (src/main.c). Today a user source that
 * declares e.g. `package dbms_output is ... end dbms_output;` fails with
 * "Duplicate package member 'dbms_output.put_line'". Desired behavior: the
 * user declaration replaces the built-in for that compilation and qualified
 * calls resolve to the user's version; packages the user does NOT declare
 * keep their built-in behavior. */

static void clean_override_db(void) {
    remove("auspex.db");
    remove("auspex.db.packages");
    remove("auspex.db.programs");
}

TEST(phase12_issue28_user_package_overrides_builtin_dbms_output) {
    clean_override_db();
    char out[512];
    /* The user-declared dbms_output replaces the built-in: put_line must
       resolve to the user body, which prints a distinguishable marker. */
    int rc = run_auspex(
        "package dbms_output is\n"
        "    proc put_line(s string) -> int;\n"
        "end dbms_output;\n"
        "\n"
        "package body dbms_output is\n"
        "    proc put_line(s string) -> int {\n"
        "        print concat(\"[custom] \", s);\n"
        "        return 0;\n"
        "    }\n"
        "end dbms_output;\n"
        "\n"
        "proc main() -> int {\n"
        "    dbms_output.put_line(\"hi\");\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "[custom] hi"));
    clean_override_db();
}

TEST(phase12_issue28_non_overridden_builtin_still_works) {
    clean_override_db();
    char out[512];
    /* Only utl_file is overridden here; the built-in dbms_output must keep
       its standard buffered behavior (enable/put_line/get_lines). */
    int rc = run_auspex(
        "package utl_file is\n"
        "    proc put_line(handle int, text string) -> int;\n"
        "end utl_file;\n"
        "\n"
        "package body utl_file is\n"
        "    proc put_line(handle int, text string) -> int {\n"
        "        print concat(\"[custom-utl] \", text);\n"
        "        return 0;\n"
        "    }\n"
        "end utl_file;\n"
        "\n"
        "proc main() -> int {\n"
        "    utl_file.put_line(0, \"fileline\");\n"
        "    dbms_output.enable(1000);\n"
        "    dbms_output.put_line(\"plain\");\n"
        "    array<string> lines = dbms_output.get_lines();\n"
        "    print length(lines);\n"
        "    print lines[0];\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "[custom-utl] fileline"));
    ASSERT_INT_EQ(1, output_contains(out, "plain"));
    ASSERT_INT_EQ(1, output_contains(out, "1"));
    clean_override_db();
}

/* ===== Issue #29: utl_file expansion =====
 *
 * The built-in utl_file package (src/packages.c) today exposes only
 * fopen/get_line/put_line/fclose over a 16-entry handle table
 * (UTL_FILE_MAX_HANDLES in src/vm.c). This pins the expanded API, all
 * implemented via VM natives and declared in the utl_file package:
 *   - fopen(path, "a") appends without truncating (libc mode passthrough,
 *     pinned here so it keeps working),
 *   - func fseek(handle int, offset int) -> int repositions the read cursor
 *     (0 on success),
 *   - proc fflush(handle int) pushes buffered writes to disk,
 *   - the handle table must hold well more than 16 open files,
 *   - func mkdir(path string) -> int and func remove(path string) -> int
 *     (0 on success, nonzero on failure). */

TEST(phase12_issue29_utl_file_append_and_fseek) {
    remove("/tmp/test_phase12_utl_append.txt");
    char out[512];
    /* Write 3 lines, append a 4th via "a" (nothing may be truncated), then
       fseek back to 0 and re-read the first line. */
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    int w = utl_file.fopen(\"/tmp/test_phase12_utl_append.txt\", \"w\");\n"
        "    utl_file.put_line(w, \"one\");\n"
        "    utl_file.put_line(w, \"two\");\n"
        "    utl_file.put_line(w, \"three\");\n"
        "    utl_file.fclose(w);\n"
        "\n"
        "    int a = utl_file.fopen(\"/tmp/test_phase12_utl_append.txt\", \"a\");\n"
        "    utl_file.put_line(a, \"four\");\n"
        "    utl_file.fclose(a);\n"
        "\n"
        "    int r = utl_file.fopen(\"/tmp/test_phase12_utl_append.txt\", \"r\");\n"
        "    string first = utl_file.get_line(r);\n"
        "    string second = utl_file.get_line(r);\n"
        "    int pos = utl_file.fseek(r, 0);\n"
        "    string again = utl_file.get_line(r);\n"
        "    utl_file.fclose(r);\n"
        "\n"
        "    print concat(\"1:\", first);\n"
        "    print concat(\"2:\", second);\n"
        "    print concat(\"seek:\", int_to_string(pos));\n"
        "    print concat(\"re:\", again);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    /* append mode must not truncate: the original first lines survive */
    ASSERT_INT_EQ(1, output_contains(out, "1:one"));
    ASSERT_INT_EQ(1, output_contains(out, "2:two"));
    /* fseek succeeded (0) and the first line is re-read after seeking */
    ASSERT_INT_EQ(1, output_contains(out, "seek:0"));
    ASSERT_INT_EQ(1, output_contains(out, "re:one"));
    remove("/tmp/test_phase12_utl_append.txt");
}

TEST(phase12_issue29_utl_file_fflush_makes_line_visible) {
    remove("/tmp/test_phase12_utl_flush.txt");
    char out[512];
    /* After put_line + fflush (handle still open, no close), the line must
       be readable from disk through the read_file native. */
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    int w = utl_file.fopen(\"/tmp/test_phase12_utl_flush.txt\", \"w\");\n"
        "    utl_file.put_line(w, \"flushed-line\");\n"
        "    utl_file.fflush(w);\n"
        "    string s = read_file(\"/tmp/test_phase12_utl_flush.txt\");\n"
        "    print s;\n"
        "    utl_file.fclose(w);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "flushed-line"));
    remove("/tmp/test_phase12_utl_flush.txt");
}

TEST(phase12_issue29_utl_file_many_open_handles) {
    char out[512];
    /* The handle table holds 16 entries today; opening 24 files at once
       must succeed once the table is grown. */
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    array<int> hs;\n"
        "    int i = 0;\n"
        "    while i < 24 {\n"
        "        int h = utl_file.fopen(\"/tmp/test_phase12_utl_many.txt\", \"a\");\n"
        "        if h < 0 {\n"
        "            print concat(\"bad-handle-at:\", int_to_string(i));\n"
        "            return 1;\n"
        "        }\n"
        "        append(hs, h);\n"
        "        i = i + 1;\n"
        "    }\n"
        "    print concat(\"opened:\", int_to_string(length(hs)));\n"
        "    i = 0;\n"
        "    while i < length(hs) {\n"
        "        utl_file.fclose(hs[i]);\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "opened:24"));
    ASSERT_INT_EQ(0, output_contains(out, "bad-handle-at:"));
    remove("/tmp/test_phase12_utl_many.txt");
}

TEST(phase12_issue29_utl_file_mkdir_and_remove) {
    char out[512];
    /* mkdir creates a directory (0 on success), remove deletes a file and
       then the now-empty directory; removing a nonexistent path fails with
       a nonzero result. */
    remove("/tmp/test_phase12_utl_dir/f.txt");
    remove("/tmp/test_phase12_utl_dir");
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    int rc = utl_file.mkdir(\"/tmp/test_phase12_utl_dir\");\n"
        "    print concat(\"mkdir:\", int_to_string(rc));\n"
        "    int h = utl_file.fopen(\"/tmp/test_phase12_utl_dir/f.txt\", \"w\");\n"
        "    utl_file.put_line(h, \"in-dir\");\n"
        "    utl_file.fclose(h);\n"
        "    int r1 = utl_file.remove(\"/tmp/test_phase12_utl_dir/f.txt\");\n"
        "    print concat(\"rmfile:\", int_to_string(r1));\n"
        "    int r2 = utl_file.remove(\"/tmp/test_phase12_utl_dir\");\n"
        "    print concat(\"rmdir:\", int_to_string(r2));\n"
        "    int r3 = utl_file.remove(\"/tmp/test_phase12_utl_dir\");\n"
        "    if r3 == 0 {\n"
        "        print \"rm-missing-returned-zero\";\n"
        "    }\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "mkdir:0"));
    ASSERT_INT_EQ(1, output_contains(out, "rmfile:0"));
    ASSERT_INT_EQ(1, output_contains(out, "rmdir:0"));
    ASSERT_INT_EQ(0, output_contains(out, "rm-missing-returned-zero"));
    /* both must actually be gone from the host filesystem */
    ASSERT_INT_EQ(0, access("/tmp/test_phase12_utl_dir/f.txt", F_OK) == 0);
    ASSERT_INT_EQ(0, access("/tmp/test_phase12_utl_dir", F_OK) == 0);
}

/* ===== Issue #30: external_call float/string marshalling =====
 *
 * external_call (src/natives.c) was hardcoded to an int(int) signature via
 * dlopen/dlsym. This test pins the two marshalling natives implemented in
 * PR #44 (named external_call_float / external_call_string there rather
 * than the _f/_s sketched in the issue comment):
 *   - func external_call_float(lib string, sym string, arg float) -> float
 *     calls a C double f(double),
 *   - func external_call_string(lib string, sym string, arg string) -> string
 *     calls a C char* s(const char*) that returns a transformed copy,
 * and regresses that the existing int external_call keeps working. The shared
 * library is built like tests/test_phase10.c does; it uses its own file name
 * so it cannot clobber the .so built by build_marshal_shared_lib above. */

static int build_phase12_shared_lib(void) {
    FILE* f = fopen("/tmp/test_phase12_ac_ext.c", "w");
    if (f == NULL) return 0;
    fprintf(f, "#include <ctype.h>\n");
    fprintf(f, "#include <stdlib.h>\n");
    fprintf(f, "#include <string.h>\n");
    fprintf(f, "int auspex_double(int x) { return x * 2; }\n");
    fprintf(f, "double auspex_triple(double x) { return x * 3.0; }\n");
    fprintf(f, "char* auspex_shout(const char* s) {\n");
    fprintf(f, "    size_t n = strlen(s);\n");
    fprintf(f, "    char* out = malloc(n + 1);\n");
    fprintf(f, "    if (out == NULL) return NULL;\n");
    fprintf(f, "    for (size_t i = 0; i < n; i++)\n");
    fprintf(f, "        out[i] = (char)toupper((unsigned char)s[i]);\n");
    fprintf(f, "    out[n] = '\\0';\n");
    fprintf(f, "    return out;\n");
    fprintf(f, "}\n");
    fclose(f);
    int rc = system("cc -shared -fPIC -o /tmp/test_phase12_ac_ext.so /tmp/test_phase12_ac_ext.c");
    return rc == 0;
}

TEST(phase12_issue30_external_call_float_and_string) {
    ASSERT_INT_EQ(1, build_phase12_shared_lib());
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    float rf = external_call_float(\"/tmp/test_phase12_ac_ext.so\", \"auspex_triple\", 1.5);\n"
        "    print concat(\"float:\", float_to_string(rf));\n"
        "    string rs = external_call_string(\"/tmp/test_phase12_ac_ext.so\", \"auspex_shout\", \"hello\");\n"
        "    print concat(\"string:\", rs);\n"
        "    int ri = external_call(\"/tmp/test_phase12_ac_ext.so\", \"auspex_double\", 21);\n"
        "    print concat(\"int:\", int_to_string(ri));\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "float:4.5"));
    ASSERT_INT_EQ(1, output_contains(out, "string:HELLO"));
    ASSERT_INT_EQ(1, output_contains(out, "int:42"));
    remove("/tmp/test_phase12_ac_ext.c");
    remove("/tmp/test_phase12_ac_ext.so");
}

/* Issue #60: get_line reads lines of any length, and past the last line it
   raises no_data_found (SQLCODE 100) instead of returning "". */
TEST(phase12_utl_file_get_line_reads_long_lines) {
    FILE* f = fopen("/tmp/test_phase12_utl_long.txt", "w");
    ASSERT_PTR_NOT_NULL(f);
    for (int i = 0; i < 5000; i++) fputc('x', f);
    fputs("END\nshort\nlast-without-newline", f);
    fclose(f);

    char out[1024];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    int r = utl_file.fopen(\"/tmp/test_phase12_utl_long.txt\", \"r\");\n"
        "    string first = utl_file.get_line(r);\n"
        "    print concat(\"len:\", int_to_string(length(first)));\n"
        "    print concat(\"tail:\", substring(first, 5000, 5003));\n"
        "    print concat(\"2:\", utl_file.get_line(r));\n"
        "    print concat(\"3:\", utl_file.get_line(r));\n"
        "    utl_file.fclose(r);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "len:5003"));
    ASSERT_INT_EQ(1, output_contains(out, "tail:END"));
    ASSERT_INT_EQ(1, output_contains(out, "2:short"));
    ASSERT_INT_EQ(1, output_contains(out, "3:last-without-newline"));
    remove("/tmp/test_phase12_utl_long.txt");
}

TEST(phase12_utl_file_get_line_raises_no_data_found_at_eof) {
    FILE* f = fopen("/tmp/test_phase12_utl_eof.txt", "w");
    ASSERT_PTR_NOT_NULL(f);
    fputs("one\n\ntwo\n", f);  /* an empty line is data, not the end */
    fclose(f);

    char out[1024];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    int r = utl_file.fopen(\"/tmp/test_phase12_utl_eof.txt\", \"r\");\n"
        "    int count = 0;\n"
        "    bool done = false;\n"
        /* Bounded, so a get_line that never raises fails fast. */
        "    for attempt in range(0, 10) {\n"
        "        if !done {\n"
        "            try {\n"
        "                string line = utl_file.get_line(r);\n"
        "                count = count + 1;\n"
        "            } catch (err) {\n"
        "                print concat(\"sqlcode:\", int_to_string(sqlcode));\n"
        "                print err;\n"
        "                done = true;\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "    utl_file.fclose(r);\n"
        "    print concat(\"lines:\", int_to_string(count));\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "lines:3"));
    ASSERT_INT_EQ(1, output_contains(out, "sqlcode:100"));
    ASSERT_INT_EQ(1, output_contains(out, "end of file (no_data_found)"));
    remove("/tmp/test_phase12_utl_eof.txt");
}

TEST(phase12_utl_file_get_line_rejects_bad_handles) {
    char out[1024];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    string s = utl_file.get_line(12);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(1, rc);
    ASSERT_INT_EQ(1, output_contains(out, "utl_file.get_line: invalid file handle"));
}

TEST(phase12_utl_file_seek_append_and_flush) {
    remove("/tmp/test_phase12_utl_file.txt");
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    int w = utl_file.fopen(\"/tmp/test_phase12_utl_file.txt\", \"w\");\n"
        "    utl_file.put_line(w, \"one\");\n"
        "    utl_file.fflush(w);\n"
        "    print read_file(\"/tmp/test_phase12_utl_file.txt\");\n"
        "    utl_file.fclose(w);\n"
        "    int a = utl_file.fopen(\"/tmp/test_phase12_utl_file.txt\", \"a\");\n"
        "    utl_file.put_line(a, \"two\");\n"
        "    utl_file.fclose(a);\n"
        "    int r = utl_file.fopen(\"/tmp/test_phase12_utl_file.txt\", \"r\");\n"
        "    string first = utl_file.get_line(r);\n"
        "    string second = utl_file.get_line(r);\n"
        "    print concat(first, second);\n"
        "    print int_to_string(utl_file.fseek(r, 0));\n"
        "    print utl_file.get_line(r);\n"
        "    utl_file.fclose(r);\n"
        "    return 0;\n"
        "}\n", out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "onetwo"));
    ASSERT_INT_EQ(1, output_contains(out, "0"));
    remove("/tmp/test_phase12_utl_file.txt");
}

TEST(phase12_utl_file_many_open_handles) {
    remove("/tmp/test_phase12_utl_many.txt");
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    array<int> handles;\n"
        "    int i = 0;\n"
        "    while i < 24 {\n"
        "        append(handles, utl_file.fopen(\"/tmp/test_phase12_utl_many.txt\", \"a\"));\n"
        "        if handles[i] < 0 { return 1; }\n"
        "        i = i + 1;\n"
        "    }\n"
        "    print int_to_string(length(handles));\n"
        "    i = 0;\n"
        "    while i < length(handles) { utl_file.fclose(handles[i]); i = i + 1; }\n"
        "    return 0;\n"
        "}\n", out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "24"));
    remove("/tmp/test_phase12_utl_many.txt");
}

TEST(phase12_utl_file_directory_operations) {
    remove("/tmp/test_phase12_utl_dir/file.txt");
    remove("/tmp/test_phase12_utl_dir");
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    print int_to_string(utl_file.mkdir(\"/tmp/test_phase12_utl_dir\"));\n"
        "    int h = utl_file.fopen(\"/tmp/test_phase12_utl_dir/file.txt\", \"w\");\n"
        "    utl_file.fclose(h);\n"
        "    print int_to_string(utl_file.remove(\"/tmp/test_phase12_utl_dir/file.txt\"));\n"
        "    print int_to_string(utl_file.remove(\"/tmp/test_phase12_utl_dir\"));\n"
        "    if utl_file.remove(\"/tmp/test_phase12_utl_dir\") == 0 { return 1; }\n"
        "    return 0;\n"
        "}\n", out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(0, access("/tmp/test_phase12_utl_dir/file.txt", F_OK) == 0);
    ASSERT_INT_EQ(0, access("/tmp/test_phase12_utl_dir", F_OK) == 0);
}

int main(void) {
    printf("test_phase12:\n");
    RUN_TEST(phase12_nextval_persists_across_restarts);
    RUN_TEST(phase12_currval_after_restart);
    RUN_TEST(phase12_drop_sequence_persists);
    RUN_TEST(phase12_currval_before_nextval_errors);
    RUN_TEST(phase12_sequence_survives_other_catalog_writes);
    RUN_TEST(phase12_duplicate_create_after_restart_errors);
    RUN_TEST(phase12_missing_sequence_errors_unchanged);
    RUN_TEST(phase12_row_trigger_insert_fires_per_row);
    RUN_TEST(phase12_row_trigger_update_sees_old_and_new);
    RUN_TEST(phase12_row_trigger_delete_sees_old);
    RUN_TEST(phase12_row_and_statement_triggers_both_fire);
    RUN_TEST(phase12_row_trigger_persists_across_restarts);
    RUN_TEST(phase12_row_trigger_fires_on_dynamic_sql);
    RUN_TEST(phase12_row_trigger_wrong_context_errors);
    RUN_TEST(phase12_row_trigger_requires_dml_event);
    RUN_TEST(phase12_trigger_static_dml_on_own_table_errors);
    RUN_TEST(phase12_row_trigger_dynamic_dml_on_own_table_errors);
    RUN_TEST(phase12_trigger_dml_through_a_proc_errors);
    RUN_TEST(phase12_trigger_cycle_through_another_table_errors);
    RUN_TEST(phase12_trigger_self_modification_error_is_catchable);
    RUN_TEST(phase12_dbms_sql_open_parse_execute_dml);
    RUN_TEST(phase12_dbms_sql_fetch_rows);
    RUN_TEST(phase12_dbms_sql_column_value);
    RUN_TEST(phase12_dbms_sql_close_invalidates_handle);
    RUN_TEST(phase12_external_call_float_return);
    RUN_TEST(phase12_external_call_int_return_from_float_and_string);
    RUN_TEST(phase12_external_call_string_return);
    RUN_TEST(phase12_external_call_string_result_is_copied);
    RUN_TEST(phase12_external_call_string_null_return_is_null);
    RUN_TEST(phase12_external_call_rejects_bool_argument);
    RUN_TEST(phase12_external_call_string_result_type_is_checked);
    RUN_TEST(phase12_external_call_string_missing_symbol_fails);
    RUN_TEST(phase12_external_call_sig_sweeps_every_signature);
    RUN_TEST(phase12_external_call_sig_checks_signature_at_compile_time);
    RUN_TEST(phase12_external_call_sig_types_its_result);
#ifdef USE_SQLITE
    RUN_TEST(phase12_dbms_sql_sqlite_bind_and_execute);
    RUN_TEST(phase12_dbms_sql_sqlite_bind_string_with_quote);
    RUN_TEST(phase12_sqlite_nextval_persists_across_restarts);
    RUN_TEST(phase12_sqlite_drop_sequence_persists);
    RUN_TEST(phase12_trigger_static_fires);
    RUN_TEST(phase12_sqlite_row_trigger_dml_on_own_table_errors);
    RUN_TEST(phase12_sqlite_row_trigger_error_message_is_reported);
    RUN_TEST(phase12_trigger_persists_across_restarts);
    RUN_TEST(phase12_drop_trigger_stops_and_persists);
    RUN_TEST(phase12_trigger_fires_on_execute_immediate);
    RUN_TEST(phase12_trigger_fires_on_dbms_sql_execute);
    RUN_TEST(phase12_drop_trigger_via_execute_immediate);
    RUN_TEST(phase12_sqlite_trigger_persists_across_restarts);
    RUN_TEST(phase12_sqlite_drop_trigger_persists);
    RUN_TEST(phase12_sqlite_row_trigger_insert_fires_per_row);
    RUN_TEST(phase12_sqlite_row_trigger_update_and_delete);
    RUN_TEST(phase12_sqlite_row_trigger_persists_across_restarts);
#endif
    RUN_TEST(phase12_issue28_user_package_overrides_builtin_dbms_output);
    RUN_TEST(phase12_issue28_non_overridden_builtin_still_works);
    RUN_TEST(phase12_issue29_utl_file_append_and_fseek);
    RUN_TEST(phase12_issue29_utl_file_fflush_makes_line_visible);
    RUN_TEST(phase12_issue29_utl_file_many_open_handles);
    RUN_TEST(phase12_issue29_utl_file_mkdir_and_remove);
    RUN_TEST(phase12_issue30_external_call_float_and_string);
    RUN_TEST(phase12_utl_file_seek_append_and_flush);
    RUN_TEST(phase12_utl_file_get_line_reads_long_lines);
    RUN_TEST(phase12_utl_file_get_line_raises_no_data_found_at_eof);
    RUN_TEST(phase12_utl_file_get_line_rejects_bad_handles);
    RUN_TEST(phase12_utl_file_many_open_handles);
    RUN_TEST(phase12_utl_file_directory_operations);
    TEST_SUMMARY();
}
