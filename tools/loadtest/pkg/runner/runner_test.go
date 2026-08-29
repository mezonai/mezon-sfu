package runner

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/gorilla/websocket"
	"mezon-sfu/tools/loadtest/pkg/report"
	"mezon-sfu/tools/loadtest/pkg/signaling"
)

var upgrader = websocket.Upgrader{
	CheckOrigin: func(r *http.Request) bool { return true },
}

func TestRunner_MockRun(t *testing.T) {
	// Set up mock signaling server that responds to joins and offers
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		c, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		defer c.Close()

		for {
			_, data, err := c.ReadMessage()
			if err != nil {
				return
			}
			var raw signaling.RawMessage
			if err := json.Unmarshal(data, &raw); err != nil {
				continue
			}

			if raw.Type == signaling.TypeJoin {
				joined := signaling.JoinedMessage{
					Type: signaling.TypeJoined,
					Room: "1000",
				}
				jBytes, _ := json.Marshal(joined)
				_ = c.WriteMessage(websocket.TextMessage, jBytes)
			}
		}
	}))
	defer server.Close()

	wsURL := "ws" + strings.TrimPrefix(server.URL, "http")

	cfg := Config{
		WSURL:           wsURL,
		JWTSecret:       "test-secret",
		Rooms:           2,
		PeersPerRoom:    2,
		SpeakersPerRoom: 1,
		Duration:        200 * time.Millisecond,
		RampDuration:    50 * time.Millisecond,
		Thresholds: report.Thresholds{
			MinSuccessRatePct: 0, // No strict requirement in unit test
		},
	}

	r := NewRunner(cfg)
	ctx, cancel := context.WithTimeout(context.Background(), 1*time.Second)
	defer cancel()

	rep, err := r.Run(ctx)
	if err != nil {
		t.Fatalf("Runner.Run failed: %v", err)
	}

	if rep.Summary.TotalRooms != 2 {
		t.Errorf("expected 2 rooms in summary, got %d", rep.Summary.TotalRooms)
	}
	if rep.Summary.TotalPeers != 4 {
		t.Errorf("expected 4 total peers in summary, got %d", rep.Summary.TotalPeers)
	}
	if rep.Summary.SpeakerCount != 2 || rep.Summary.AudienceCount != 2 {
		t.Errorf("expected 2 speakers, 2 audience, got %d / %d",
			rep.Summary.SpeakerCount, rep.Summary.AudienceCount)
	}
}

func TestRunner_Cancellation(t *testing.T) {
	cfg := Config{
		WSURL:           "ws://127.0.0.1:19999/ws", // Non-existent
		JWTSecret:       "test-secret",
		Rooms:           5,
		PeersPerRoom:    5,
		SpeakersPerRoom: 1,
		Duration:        10 * time.Second,
		RampDuration:    2 * time.Second,
	}

	r := NewRunner(cfg)
	ctx, cancel := context.WithCancel(context.Background())

	go func() {
		time.Sleep(50 * time.Millisecond)
		cancel()
	}()

	_, err := r.Run(ctx)
	if err == nil {
		t.Errorf("expected context cancellation error, got nil")
	}
}
