// Known-answer SQL-level checks of sqlite-vec: every result is compared with a hand-computed value.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "sqlite-vec.h"
#include "sqlite3.h"

static int passed = 0, failed = 0;
static sqlite3 *db;

static void check(const char *name, int cond) {
  if (cond) { passed++; printf("PASS %s\n", name); }
  else { failed++; printf("FAIL %s\n", name); }
}

static int ok(const char *sql) { return sqlite3_exec(db, sql, 0, 0, 0) == SQLITE_OK; }
static int fails(const char *sql) { return sqlite3_exec(db, sql, 0, 0, 0) != SQLITE_OK; }

static double num(const char *sql) {
  sqlite3_stmt *s;
  double v = NAN;
  if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW)
    v = sqlite3_column_double(s, 0);
  sqlite3_finalize(s);
  return v;
}

// All values of the first column, comma-joined ("2,1,3,"); "ERR" if the statement fails.
static const char *col(const char *sql) {
  static char out[256];
  sqlite3_stmt *s;
  int rc = SQLITE_ERROR;
  out[0] = 0;
  if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
      const char *t = (const char *)sqlite3_column_text(s, 0);
      if (strlen(out) + strlen(t) + 2 < sizeof out) { strcat(out, t); strcat(out, ","); }
    }
  }
  sqlite3_finalize(s);
  if (rc != SQLITE_DONE) return "ERR";
  return out;
}

static int eq(const char *sql, const char *want) { return strcmp(col(sql), want) == 0; }
static int near(const char *sql, double want) { return fabs(num(sql) - want) < 1e-4; }

int main(void) {
  sqlite3_open(":memory:", &db);
  check("init", sqlite3_vec_init(db, NULL, NULL) == SQLITE_OK);

  // scalar distances and vector functions
  check("l2", near("select vec_distance_l2('[1,2,3]','[4,6,3]')", 5.0));
  check("l1", near("select vec_distance_l1('[1,2,3]','[4,6,3]')", 7.0));
  check("cosine orthogonal", near("select vec_distance_cosine('[1,0]','[0,1]')", 1.0));
  check("cosine identical", near("select vec_distance_cosine('[1,1]','[1,1]')", 0.0));
  check("l2 int8", near("select vec_distance_l2(vec_int8('[1,2,3]'), vec_int8('[4,6,3]'))", 5.0));
  check("hamming all", near("select vec_distance_hamming(vec_bit(X'FF'), vec_bit(X'00'))", 8.0));
  check("hamming none", near("select vec_distance_hamming(vec_bit(X'A5'), vec_bit(X'A5'))", 0.0));
  check("length", near("select vec_length('[1,2,3,4]')", 4.0));
  check("type", eq("select vec_type(vec_int8('[1,2]'))", "int8,"));
  check("to_json", eq("select vec_to_json(vec_f32('[1,2]'))", "[1.000000,2.000000],"));
  check("add", eq("select vec_to_json(vec_add('[1,2]','[3,4]'))", "[4.000000,6.000000],"));
  check("sub", eq("select vec_to_json(vec_sub('[5,5]','[1,2]'))", "[4.000000,3.000000],"));
  check("slice", eq("select vec_to_json(vec_slice('[1,2,3,4]',1,3))", "[2.000000,3.000000],"));
  check("normalize", near("select vec_distance_l2(vec_normalize('[3,4]'),'[0.6,0.8]')", 0.0));
  check("quantize binary ones", eq("select hex(vec_quantize_binary('[1,1,1,1,1,1,1,1]'))", "FF,"));
  check("quantize binary negatives", eq("select hex(vec_quantize_binary('[-1,-1,-1,-1,-1,-1,-1,-1]'))", "00,"));
  check("dimension mismatch errors", fails("select vec_distance_l2('[1,2]','[1,2,3]')"));
  check("bad json errors", fails("select vec_length('not json')"));

  // float32 table: knn, delete, update
  check("create float", ok("create virtual table vf using vec0(emb float[2])"));
  check("insert float", ok("insert into vf(rowid, emb) values (1,'[0,0]'),(2,'[1,1]'),(3,'[5,5]'),(4,'[10,10]')"));
  check("knn order", eq("select rowid from vf where emb match '[1.2,1.2]' and k = 3 order by distance", "2,1,3,"));
  check("knn distance", near("select distance from vf where emb match '[3,4]' and k = 1", 2.236068));
  check("wrong dimension insert errors", fails("insert into vf(rowid, emb) values (9,'[1,2,3]')"));
  check("delete", ok("delete from vf where rowid = 2"));
  check("knn after delete", eq("select rowid from vf where emb match '[1.2,1.2]' and k = 2 order by distance", "1,3,"));
  check("count", near("select count(*) from vf", 3.0));
  check("update", ok("update vf set emb = '[100,100]' where rowid = 1"));
  check("knn after update", eq("select rowid from vf where emb match '[99,99]' and k = 1", "1,"));

  // other element types and metrics
  check("create cosine", ok("create virtual table vc using vec0(emb float[2] distance_metric=cosine)"));
  check("insert cosine", ok("insert into vc(rowid, emb) values (1,'[1,0]'),(2,'[0,1]'),(3,'[1,1]')"));
  check("knn cosine", eq("select rowid from vc where emb match '[1,0.1]' and k = 3 order by distance", "1,3,2,"));
  check("create int8", ok("create virtual table vi using vec0(emb int8[2])"));
  check("insert int8", ok("insert into vi(rowid, emb) values (1,vec_int8('[1,1]')),(2,vec_int8('[10,10]')),(3,vec_int8('[-5,-5]'))"));
  check("knn int8", eq("select rowid from vi where emb match vec_int8('[2,2]') and k = 3 order by distance", "1,3,2,"));
  check("create bit", ok("create virtual table vb using vec0(emb bit[8])"));
  check("insert bit", ok("insert into vb(rowid, emb) values (1,vec_bit(X'FF')),(2,vec_bit(X'0F')),(3,vec_bit(X'00'))"));
  check("knn bit", eq("select rowid from vb where emb match vec_bit(X'0E') and k = 3 order by distance", "2,3,1,"));

  // partition keys and metadata columns
  check("create partition", ok("create virtual table vp using vec0(part int partition key, emb float[2])"));
  check("insert partition", ok("insert into vp(rowid, part, emb) values (1,1,'[0,0]'),(2,1,'[1,1]'),(3,2,'[0.1,0.1]')"));
  check("knn partition 1", eq("select rowid from vp where emb match '[0,0]' and k = 2 and part = 1 order by distance", "1,2,"));
  check("knn partition 2", eq("select rowid from vp where emb match '[0,0]' and k = 2 and part = 2", "3,"));
  check("create metadata", ok("create virtual table vm using vec0(emb float[2], tag text)"));
  check("insert metadata", ok("insert into vm(rowid, emb, tag) values (1,'[0,0]','a'),(2,'[1,1]','b'),(3,'[2,2]','a')"));
  check("knn metadata a", eq("select rowid from vm where emb match '[0.9,0.9]' and k = 3 and tag = 'a' order by distance", "1,3,"));
  check("knn metadata b", eq("select rowid from vm where emb match '[0.9,0.9]' and k = 3 and tag = 'b'", "2,"));

  // indexed variants
  check("create rescore", ok("create virtual table vr using vec0(emb float[8] indexed by rescore(quantizer=int8))"));
  check("insert rescore", ok("insert into vr(rowid, emb) values "
        "(1,'[1,1,1,1,1,1,1,1]'),(2,'[2,2,2,2,2,2,2,2]'),(3,'[9,9,9,9,9,9,9,9]')"));
  check("knn rescore", eq("select rowid from vr where emb match '[2.1,2.1,2.1,2.1,2.1,2.1,2.1,2.1]' and k = 2 order by distance", "2,1,"));
  check("create diskann", ok("create virtual table vd using vec0(emb float[8] indexed by diskann(neighbor_quantizer=binary))"));
  check("insert diskann", ok("insert into vd(rowid, emb) values "
        "(1,'[1,-1,-1,-1,-1,-1,-1,-1]'),(2,'[-1,1,-1,-1,-1,-1,-1,-1]'),(3,'[-1,-1,1,-1,-1,-1,-1,-1]'),"
        "(4,'[-1,-1,-1,1,-1,-1,-1,-1]'),(5,'[-1,-1,-1,-1,1,-1,-1,-1]')"));
  check("knn diskann", eq("select rowid from vd where emb match '[-1,-1,1,-1,-1,-1,-1,-1]' and k = 1", "3,"));

  sqlite3_close(db);
  printf("SQLTEST passed=%d failed=%d\n", passed, failed);
  return failed ? 1 : 0;
}
