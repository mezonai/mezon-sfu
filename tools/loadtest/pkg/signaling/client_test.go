package signaling

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

var upgrader = websocket.Upgrader{
	CheckOrigin: func(r *http.Request) bool { return true },
}

func TestSignalingClient_ConnectJoinAndOfferAnswer(t *testing.T) {
	offerSDP := "v=0\r\no=- 12345 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
	expectedGen := uint64(42)

	var answerReceived sync.WaitGroup
	answerReceived.Add(1)

	var receivedAnswer AnswerMessage
	var receivedJoin JoinMessage
	_ = receivedJoin

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		c, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			t.Errorf("upgrade failed: %v", err)
			return
		}
		defer c.Close()

		for {
			_, data, err := c.ReadMessage()
			if err != nil {
				return
			}
			var raw RawMessage
			if err := json.Unmarshal(data, &raw); err != nil {
				continue
			}

			switch raw.Type {
			case TypeJoin:
				json.Unmarshal(data, &receivedJoin)
				// Send joined
				joined := JoinedMessage{
					Type: TypeJoined,
					Room: "101",
				}
				joinedBytes, _ := json.Marshal(joined)
				c.WriteMessage(websocket.TextMessage, joinedBytes)

				// Send offer
				offer := OfferMessage{
					Type:            TypeOffer,
					OfferGeneration: expectedGen,
					SDP:             offerSDP,
				}
				offerBytes, _ := json.Marshal(offer)
				c.WriteMessage(websocket.TextMessage, offerBytes)

			case TypeAnswer:
				json.Unmarshal(data, &receivedAnswer)
				answerReceived.Done()

			case TypePing:
				pong := map[string]string{"type": "pong"}
				pongBytes, _ := json.Marshal(pong)
				c.WriteMessage(websocket.TextMessage, pongBytes)
			}
		}
	}))
	defer server.Close()

	wsURL := "ws" + strings.TrimPrefix(server.URL, "http")

	var offerHandled sync.WaitGroup
	offerHandled.Add(1)

	var clientOffer OfferMessage
	var clientJoined JoinedMessage

	handlers := ClientHandlers{
		OnJoined: func(msg JoinedMessage) {
			clientJoined = msg
		},
		OnOffer: func(msg OfferMessage) {
			clientOffer = msg
			offerHandled.Done()
		},
	}

	client := NewClient(wsURL, handlers)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	if err := client.Connect(ctx); err != nil {
		t.Fatalf("client.Connect failed: %v", err)
	}
	defer client.Close()

	if err := client.Join("test-token-123", "speaker", "vp8"); err != nil {
		t.Fatalf("client.Join failed: %v", err)
	}

	offerHandled.Wait()

	if clientJoined.Room != "101" {
		t.Errorf("expected room 101, got %s", clientJoined.Room)
	}
	if clientOffer.OfferGeneration != expectedGen || clientOffer.SDP != offerSDP {
		t.Errorf("unexpected offer: %+v", clientOffer)
	}

	// Send Answer back
	localSDP := "v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
	if err := client.SendAnswer(localSDP, clientOffer.OfferGeneration); err != nil {
		t.Fatalf("SendAnswer failed: %v", err)
	}

	answerReceived.Wait()

	if receivedAnswer.OfferGeneration != expectedGen || receivedAnswer.SDP != localSDP {
		t.Errorf("server received invalid answer: %+v", receivedAnswer)
	}
	if receivedJoin.Role != "speaker" || receivedJoin.ScreenCodec != "vp8" {
		t.Errorf("server received invalid join: %+v", receivedJoin)
	}
}
