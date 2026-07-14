#include "protocol/signaling/sdp.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *SAMPLE_OFFER =
"v=0\r\n"
"o=- 4611731400430051336 2 IN IP4 127.0.0.1\r\n"
"s=-\r\n"
"t=0 0\r\n"
"a=group:BUNDLE 0\r\n"
"a=extmap-allow-mixed\r\n"
"a=msid-semantic: WMS stream1\r\n"
"m=audio 9 UDP/TLS/RTP/SAVPF 111 63 9 0 8 13 110 126\r\n"
"c=IN IP4 0.0.0.0\r\n"
"a=rtcp:9 IN IP4 0.0.0.0\r\n"
"a=ice-ufrag:browserUfrag1\r\n"
"a=ice-pwd:browserPasswordValueGoesHereXXXX\r\n"
"a=ice-options:trickle\r\n"
"a=fingerprint:sha-256 AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99\r\n"
"a=setup:actpass\r\n"
"a=mid:0\r\n"
"a=extmap:1 urn:ietf:params:rtp-hdrext:ssrc-audio-level\r\n"
"a=sendrecv\r\n"
"a=rtcp-mux\r\n"
"a=rtpmap:111 opus/48000/2\r\n"
"a=rtcp-fb:111 transport-cc\r\n"
"a=fmtp:111 minptime=10;useinbandfec=1\r\n"
"a=rtpmap:63 red/48000/2\r\n"
"a=fmtp:63 111/111\r\n"
"a=rtpmap:9 G722/8000\r\n"
"a=rtpmap:0 PCMU/8000\r\n"
"a=rtpmap:8 PCMA/8000\r\n"
"a=rtpmap:13 CN/8000\r\n"
"a=rtpmap:110 telephone-event/48000\r\n"
"a=rtpmap:126 telephone-event/8000\r\n"
"a=ssrc:1234567890 cname:testcname\r\n"
"a=candidate:1 1 udp 2122260223 192.168.1.5 54321 typ host generation 0\r\n"
"a=candidate:2 1 udp 1686052607 203.0.113.9 54322 typ srflx raddr 192.168.1.5 rport 54321 generation 0\r\n"
"a=end-of-candidates\r\n";

static int contains(const char *haystack, const char *needle) {
    return strstr(haystack, needle) != NULL;
}

static int count_occurrences(const char *haystack, const char *needle) {
    int n = 0;
    const char *p = haystack;
    size_t nlen = strlen(needle);
    while ((p = strstr(p, needle)) != NULL) { n++; p += nlen; }
    return n;
}

int main(void) {
    char answer[8192];
    int len = sfu_sdp_build_answer(SAMPLE_OFFER, strlen(SAMPLE_OFFER),
                                    "127.0.0.1", 17030,
                                    "XKrsH3xm", "dHkzP4aajGOJsWhquFzy3pxr",
                                    "32:01:9A:1C:1F:71:54:36:78:9C:AD:50:B8:93:2D:A9:B9:FC:A5:C1:94:C0:C6:80:7A:03:87:B5:F5:1F:F3",
                                    answer, sizeof(answer));
    assert(len > 0);
    answer[len] = '\0';

    struct { const char *desc; int ok; } checks[] = {
        { "contains our ufrag", contains(answer, "a=ice-ufrag:XKrsH3xm") },
        { "contains our pwd", contains(answer, "a=ice-pwd:dHkzP4aajGOJsWhquFzy3pxr") },
        { "contains our fingerprint", contains(answer, "a=fingerprint:sha-256 32:01:9A") },
        { "setup:passive present", contains(answer, "a=setup:passive") },
        { "no setup:actpass leaked", !contains(answer, "actpass") },
        { "exactly one candidate line", count_occurrences(answer, "a=candidate:") == 1 },
        { "our candidate uses our host:port",
          contains(answer, "a=candidate:1 1 udp 2130706431 127.0.0.1 17030 typ host") },
        { "no browser candidates leaked", !contains(answer, "192.168.1.5") && !contains(answer, "203.0.113.9") },
        { "opus rtpmap preserved", contains(answer, "a=rtpmap:111 opus/48000/2") },
        { "fmtp preserved", contains(answer, "a=fmtp:111 minptime=10;useinbandfec=1") },
        { "mid preserved", contains(answer, "a=mid:0") },
        { "sendrecv preserved", contains(answer, "a=sendrecv") },
        { "rtcp-mux preserved", contains(answer, "a=rtcp-mux") },
        { "BUNDLE group preserved", contains(answer, "a=group:BUNDLE 0") },
        { "m=audio port replaced", contains(answer, "m=audio 17030 UDP/TLS/RTP/SAVPF") },
        { "c= line uses our host", contains(answer, "c=IN IP4 127.0.0.1") },
        { "end-of-candidates present", contains(answer, "a=end-of-candidates") },
        { "no browser ice-options:trickle leaked", !contains(answer, "ice-options:trickle") },
    };

    int all_ok = 1;
    for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
        printf("%s - %s\n", checks[i].ok ? "PASS" : "FAIL", checks[i].desc);
        if (!checks[i].ok) all_ok = 0;
    }
    assert(all_ok);

    /* An offer with no m= line must fail cleanly, not crash or emit
     * a bogus answer. */
    const char *no_media = "v=0\r\no=- 1 2 IN IP4 1.2.3.4\r\ns=-\r\nt=0 0\r\n";
    assert(sfu_sdp_build_answer(no_media, strlen(no_media), "127.0.0.1", 17030,
                                 "u", "p", "AA:BB", answer, sizeof(answer)) == -1);

    printf("test_sdp: OK\n");
    return 0;
}
