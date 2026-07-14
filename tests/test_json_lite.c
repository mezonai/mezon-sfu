#include "protocol/signaling/json_lite.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    /* Basic extraction. */
    {
        char out[64];
        int n = sfu_json_extract_string("{\"type\":\"offer\",\"sdp\":\"hello\"}", "type", out, sizeof(out));
        assert(n == 5);
        assert(strcmp(out, "offer") == 0);
    }

    /* Field with escaped CRLF, as a real SDP payload would arrive after
     * JSON.stringify() on the client side. */
    {
        const char *json = "{\"type\":\"offer\",\"sdp\":\"v=0\\r\\no=- 1 2 IN IP4 1.2.3.4\\r\\n\"}";
        char out[128];
        int n = sfu_json_extract_string(json, "sdp", out, sizeof(out));
        assert(n > 0);
        assert(strcmp(out, "v=0\r\no=- 1 2 IN IP4 1.2.3.4\r\n") == 0);
    }

    /* Missing field. */
    {
        char out[16];
        assert(sfu_json_extract_string("{\"type\":\"offer\"}", "sdp", out, sizeof(out)) == -1);
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
        int rlen = sfu_json_extract_string(json, "sdp", roundtrip, sizeof(roundtrip));
        assert(rlen == (int)strlen(original));
        assert(memcmp(roundtrip, original, (size_t)rlen) == 0);
    }

    /* Overflow must fail cleanly, not corrupt/truncate silently past cap. */
    {
        char tiny[4];
        int n = sfu_json_extract_string("{\"sdp\":\"toolong\"}", "sdp", tiny, sizeof(tiny));
        assert(n == -1);
    }

    printf("test_json_lite: OK\n");
    return 0;
}
