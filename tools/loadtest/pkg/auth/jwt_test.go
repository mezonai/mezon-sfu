package auth

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"strings"
	"testing"
	"time"
)

func TestGenerateToken(t *testing.T) {
	secret := "test-secret-key-1234567890"
	userID := int64(42)
	roomID := uint64(101)

	token, err := GenerateToken(secret, userID, roomID, time.Hour)
	if err != nil {
		t.Fatalf("GenerateToken failed: %v", err)
	}

	parts := strings.Split(token, ".")
	if len(parts) != 3 {
		t.Fatalf("expected 3 JWT parts, got %d", len(parts))
	}

	// Verify Header
	hdrBytes, err := base64.RawURLEncoding.DecodeString(parts[0])
	if err != nil {
		t.Fatalf("failed to decode header: %v", err)
	}
	var hdr map[string]string
	if err := json.Unmarshal(hdrBytes, &hdr); err != nil {
		t.Fatalf("failed to parse header JSON: %v", err)
	}
	if hdr["alg"] != "HS256" || hdr["typ"] != "JWT" {
		t.Errorf("unexpected header: %v", hdr)
	}

	// Verify Payload
	payloadBytes, err := base64.RawURLEncoding.DecodeString(parts[1])
	if err != nil {
		t.Fatalf("failed to decode payload: %v", err)
	}
	var claims Claims
	if err := json.Unmarshal(payloadBytes, &claims); err != nil {
		t.Fatalf("failed to parse payload JSON: %v", err)
	}
	if claims.Identity != "42" || claims.Sub != "42" {
		t.Errorf("unexpected identity/sub: %s/%s", claims.Identity, claims.Sub)
	}
	if claims.Room != roomID {
		t.Errorf("unexpected room: %d", claims.Room)
	}
	if claims.Video == nil || claims.Video.Room != "101" {
		t.Errorf("unexpected video claim: %+v", claims.Video)
	}

	// Verify Signature
	signingInput := parts[0] + "." + parts[1]
	mac := hmac.New(sha256.New, []byte(secret))
	mac.Write([]byte(signingInput))
	expectedSig := mac.Sum(nil)
	actualSig, err := base64.RawURLEncoding.DecodeString(parts[2])
	if err != nil {
		t.Fatalf("failed to decode signature: %v", err)
	}
	if !hmac.Equal(actualSig, expectedSig) {
		t.Errorf("signature verification failed")
	}
}

func TestGenerateToken_EmptySecret(t *testing.T) {
	_, err := GenerateToken("", 1, 1, time.Hour)
	if err == nil {
		t.Fatalf("expected error for empty secret, got nil")
	}
}
