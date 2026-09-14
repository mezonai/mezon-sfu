#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "config/config.h"

static void write_temp_file(const char *path, const char *content) {
  FILE *fp = fopen(path, "w");
  assert(fp != NULL);
  fputs(content, fp);
  fclose(fp);
}

static void test_config_defaults(void) {
  sfu_config_set_defaults();
  assert(g_sfu_config.alone_participant_timeout_seconds == 1800);
  assert(sfu_config_validate(&g_sfu_config) == 0);
}

static void test_config_load_repo_ini(void) {
  const char *candidate_paths[] = {
      "config.ini",
      "../config.ini",
      "../../config.ini",
      NULL
  };
  const char *path = NULL;
  for (int i = 0; candidate_paths[i]; i++) {
    if (access(candidate_paths[i], R_OK) == 0) {
      path = candidate_paths[i];
      break;
    }
  }
  assert(path != NULL);
  int rc = sfu_config_load_ini(path);
  assert(rc == 0);
  assert(g_sfu_config.alone_participant_timeout_seconds == 1800);
  assert(sfu_config_validate(&g_sfu_config) == 0);
}

static void test_config_custom_alone_timeout(void) {
  const char *tmp_path = "test_custom_config.tmp.ini";
  const char *ini_content =
      "[room]\n"
      "alone_participant_timeout_seconds = 600\n";
  write_temp_file(tmp_path, ini_content);

  int rc = sfu_config_load_ini(tmp_path);
  unlink(tmp_path);
  assert(rc == 0);
  assert(g_sfu_config.alone_participant_timeout_seconds == 600);
  assert(sfu_config_validate(&g_sfu_config) == 0);
}

static void test_config_alone_timeout_disabled(void) {
  const char *tmp_path = "test_disabled_config.tmp.ini";
  const char *ini_content =
      "[room]\n"
      "alone_participant_timeout_seconds = 0\n";
  write_temp_file(tmp_path, ini_content);

  int rc = sfu_config_load_ini(tmp_path);
  unlink(tmp_path);
  assert(rc == 0);
  assert(g_sfu_config.alone_participant_timeout_seconds == 0);
  assert(sfu_config_validate(&g_sfu_config) == 0);
}

static void test_config_alone_timeout_aliases(void) {
  const char *tmp_path = "test_aliases_config.tmp.ini";

  write_temp_file(tmp_path, "[room]\nalone_timeout = 300\n");
  assert(sfu_config_load_ini(tmp_path) == 0);
  assert(g_sfu_config.alone_participant_timeout_seconds == 300);

  write_temp_file(tmp_path, "[room]\nalone_timeout_sec = 450\n");
  assert(sfu_config_load_ini(tmp_path) == 0);
  assert(g_sfu_config.alone_participant_timeout_seconds == 450);

  unlink(tmp_path);
}

static void test_config_alone_timeout_section_scoping(void) {
  const char *tmp_path = "test_scoping_config.tmp.ini";
  /* Under [server] instead of [room], should be ignored and keep default */
  const char *ini_content =
      "[server]\n"
      "alone_participant_timeout_seconds = 999\n";
  write_temp_file(tmp_path, ini_content);

  int rc = sfu_config_load_ini(tmp_path);
  unlink(tmp_path);
  assert(rc == 0);
  assert(g_sfu_config.alone_participant_timeout_seconds == 1800);
}

static void test_config_alone_timeout_invalid_negative(void) {
  const char *tmp_path = "test_neg_config.tmp.ini";
  const char *ini_content =
      "[room]\n"
      "alone_participant_timeout_seconds = -1\n";
  write_temp_file(tmp_path, ini_content);

  int rc = sfu_config_load_ini(tmp_path);
  unlink(tmp_path);
  assert(rc != 0);
  assert(sfu_config_validate(&g_sfu_config) != 0);
}

static void test_config_alone_timeout_invalid_text(void) {
  const char *tmp_path = "test_text_config.tmp.ini";
  const char *ini_content =
      "[room]\n"
      "alone_participant_timeout_seconds = abc\n";
  write_temp_file(tmp_path, ini_content);

  int rc = sfu_config_load_ini(tmp_path);
  unlink(tmp_path);
  assert(rc != 0);
  assert(sfu_config_validate(&g_sfu_config) != 0);
}

static void test_config_alone_timeout_overflow(void) {
  const char *tmp_path = "test_overflow_config.tmp.ini";
  const char *ini_content =
      "[room]\n"
      "alone_participant_timeout_seconds = 9999999999999999999999\n";
  write_temp_file(tmp_path, ini_content);

  int rc = sfu_config_load_ini(tmp_path);
  unlink(tmp_path);
  assert(rc != 0);
  assert(sfu_config_validate(&g_sfu_config) != 0);
}

static void test_config_alone_timeout_validation_boundary(void) {
  sfu_config_set_defaults();
  g_sfu_config.alone_participant_timeout_seconds = 86400; /* 24 hours: allowed max */
  assert(sfu_config_validate(&g_sfu_config) == 0);

  g_sfu_config.alone_participant_timeout_seconds = 86401; /* Exceeds max */
  assert(sfu_config_validate(&g_sfu_config) != 0);
}

int main(void) {
  test_config_defaults();
  test_config_load_repo_ini();
  test_config_custom_alone_timeout();
  test_config_alone_timeout_disabled();
  test_config_alone_timeout_aliases();
  test_config_alone_timeout_section_scoping();
  test_config_alone_timeout_invalid_negative();
  test_config_alone_timeout_invalid_text();
  test_config_alone_timeout_overflow();
  test_config_alone_timeout_validation_boundary();
  printf("test_config: OK\n");
  return 0;
}
