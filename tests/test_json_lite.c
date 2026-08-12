#include "protocol/signaling/json_lite.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  /* Basic extraction. */
  {
    char out[64];
    int n = sfu_json_extract_string("{\"type\":\"offer\",\"sdp\":\"hello\"}", strlen("{\"type\":\"offer\",\"sdp\":\"hello\"}"), "type", out, sizeof(out));
    assert(n == 5);
    assert(strcmp(out, "offer") == 0);
  }

  /* Field with escaped CRLF, as a real SDP payload would arrive after
   * JSON.stringify() on the client side. */
  {
    const char *json =
        "{\"type\":\"offer\",\"sdp\":\"v=0\\r\\no=- 1 2 IN IP4 "
        "1.2.3.4\\r\\n\"}";
    char out[128];
    int n = sfu_json_extract_string(json, strlen(json), "sdp", out, sizeof(out));
    assert(n > 0);
    assert(strcmp(out, "v=0\r\no=- 1 2 IN IP4 1.2.3.4\r\n") == 0);
  }

  /* Missing field. */
  {
    char out[16];
    assert(sfu_json_extract_string("{\"type\":\"offer\"}", strlen("{\"type\":\"offer\"}"), "sdp", out, sizeof(out)) == -1);
  }

  /* Escape round-trips back to the original through extraction. */
  {
    const char *original = "line1\r\nline2\twith \"quotes\" and \\backslash\\\r\n";
    char escaped[256];
    int elen = sfu_json_escape(original, strlen(original), escaped, sizeof(escaped));
    assert(elen > 0);

    char json[320];
    snprintf(json, sizeof(json), "{\"sdp\":\"%s\"}", escaped);

    char roundtrip[256];
    int rlen = sfu_json_extract_string(json, strlen(json), "sdp", roundtrip, sizeof(roundtrip));
    assert(rlen == (int)strlen(original));
    assert(memcmp(roundtrip, original, (size_t)rlen) == 0);
  }

  /* Overflow must fail cleanly, not corrupt/truncate silently past cap. */
  {
    char tiny[4];
    int n = sfu_json_extract_string("{\"sdp\":\"toolong\"}", strlen("{\"sdp\":\"toolong\"}"), "sdp", tiny, sizeof(tiny));
    assert(n == -1);
  }

  /* The parser must not require NUL termination. */
  {
    const char json[] = {'{', '"', 's', 'd', 'p', '"', ':', '"', 'o', 'k', '"', '}'};
    char out[8];
    assert(sfu_json_extract_string(json, sizeof(json), "sdp", out, sizeof(out)) == 2);
    assert(strcmp(out, "ok") == 0);
  }

  /* A trailing escape at the explicit boundary is malformed and must not
   * inspect the byte just beyond that boundary. */
  {
    const char json[] = {'{', '"', 's', 'd', 'p', '"', ':', '"', 'x', '\\', '"', '}'};
    char out[8];
    assert(sfu_json_extract_string(json, 10, "sdp", out, sizeof(out)) == -1);
  }

  /* Empty-string field with out_cap=0 must reject before any write. */
  {
    unsigned char canary = 0xA5;
    int n = sfu_json_extract_string("{\"sdp\":\"\"}", strlen("{\"sdp\":\"\"}"), "sdp", (char *)&canary, 0);
    assert(n == -1);
    assert(canary == 0xA5);
  }

  /* out_cap=1 with empty string succeeds with just the trailing NUL. */
  {
    char out = 0x5A;
    int n = sfu_json_extract_string("{\"sdp\":\"\"}", strlen("{\"sdp\":\"\"}"), "sdp", &out, 1);
    assert(n == 0);
    assert(out == '\0');
  }

  /* Normal extraction of a non-empty string field. */
  {
    char out[16];
    int n = sfu_json_extract_string("{\"room\":\"abc\"}", strlen("{\"room\":\"abc\"}"), "room", out, sizeof(out));
    assert(n == 3);
    assert(strcmp(out, "abc") == 0);
  }

  {
    bool v = false;
    assert(sfu_json_extract_bool("{\"type\":\"visibility\",\"visible\":true}", strlen("{\"type\":\"visibility\",\"visible\":true}"), "visible", &v) == 0);
    assert(v == true);
    assert(sfu_json_extract_bool("{\"visible\":false}", strlen("{\"visible\":false}"), "visible", &v) == 0);
    assert(v == false);
  }

  {
    bool v = false;
    assert(sfu_json_extract_bool("{\"visible\":\"true\"}", strlen("{\"visible\":\"true\"}"), "visible", &v) == 0);
    assert(v == true);
    assert(sfu_json_extract_bool("{\"visible\":\"0\"}", strlen("{\"visible\":\"0\"}"), "visible", &v) == 0);
    assert(v == false);
  }

  {
    bool v = true;
    assert(sfu_json_extract_bool("{\"type\":\"visibility\"}", strlen("{\"type\":\"visibility\"}"), "visible", &v) == -1);
    assert(sfu_json_extract_bool("{\"visible\":\"maybe\"}", strlen("{\"visible\":\"maybe\"}"), "visible", &v) == -1);
    assert(sfu_json_extract_bool(NULL, 0, "visible", &v) == -1);
  }

  /* Escape with out == NULL must return -1 without writing. */
  {
    assert(sfu_json_escape("x", 1, NULL, 8) == -1);
    assert(sfu_json_escape("", 0, NULL, 1) == -1);
  }

  /* Escape with out_cap == 0 must return -1 and leave the canary untouched. */
  {
    unsigned char canary = 0xA5;
    assert(sfu_json_escape("", 0, (char *)&canary, 0) == -1);
    assert(canary == 0xA5);
  }

  printf("test_json_lite: OK\n");
  return 0;
}
