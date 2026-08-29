package auth

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"time"
)

// Claims represents the JWT claims payload required by mezon-sfu.
type Claims struct {
	Identity string     `json:"identity,omitempty"`
	Sub      string     `json:"sub,omitempty"`
	Exp      int64      `json:"exp"`
	Nbf      int64      `json:"nbf"`
	Iss      string     `json:"iss,omitempty"`
	Metadata string     `json:"metadata,omitempty"`
	Room     uint64     `json:"room,omitempty"`
	Video    *VideoData `json:"video,omitempty"`
}

// VideoData represents the nested video claims object.
type VideoData struct {
	Room     string `json:"room,omitempty"`
	RoomJoin bool   `json:"roomJoin,omitempty"`
}

type header struct {
	Alg string `json:"alg"`
	Typ string `json:"typ"`
}

// GenerateToken creates an HS256 JWT token valid for the given userId and roomId.
func GenerateToken(secret string, userID int64, roomID uint64, ttl time.Duration) (string, error) {
	if secret == "" {
		return "", fmt.Errorf("jwt secret cannot be empty")
	}
	now := time.Now().Unix()
	if ttl <= 0 {
		ttl = 24 * time.Hour
	}

	claims := Claims{
		Identity: fmt.Sprintf("%d", userID),
		Sub:      fmt.Sprintf("%d", userID),
		Exp:      now + int64(ttl.Seconds()),
		Nbf:      now - 60, // allow 60s clock skew
		Iss:      "mezon-sfu-loadtest",
		Metadata: "",
		Room:     roomID,
		Video: &VideoData{
			Room:     fmt.Sprintf("%d", roomID),
			RoomJoin: true,
		},
	}

	return SignToken(secret, claims)
}

// SignToken serializes and signs any Claims struct with HS256 HMAC.
func SignToken(secret string, claims Claims) (string, error) {
	hdr := header{
		Alg: "HS256",
		Typ: "JWT",
	}

	hdrJSON, err := json.Marshal(hdr)
	if err != nil {
		return "", fmt.Errorf("failed to marshal header: %w", err)
	}

	payloadJSON, err := json.Marshal(claims)
	if err != nil {
		return "", fmt.Errorf("failed to marshal payload: %w", err)
	}

	enc := base64.RawURLEncoding
	hdrB64 := enc.EncodeToString(hdrJSON)
	payloadB64 := enc.EncodeToString(payloadJSON)

	signingInput := hdrB64 + "." + payloadB64

	mac := hmac.New(sha256.New, []byte(secret))
	if _, err := mac.Write([]byte(signingInput)); err != nil {
		return "", fmt.Errorf("failed to compute hmac: %w", err)
	}
	signature := enc.EncodeToString(mac.Sum(nil))

	return signingInput + "." + signature, nil
}
