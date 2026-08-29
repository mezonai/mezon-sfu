package signaling

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"sync"
	"sync/atomic"
	"time"

	"github.com/gorilla/websocket"
)

var (
	ErrClientClosed = errors.New("signaling client is closed")
)

// ClientHandlers defines callback functions for incoming signaling messages.
type ClientHandlers struct {
	OnJoined      func(msg JoinedMessage)
	OnOffer       func(msg OfferMessage)
	OnPeerUpdated func(msg PeerUpdatedMessage)
	OnError       func(msg ErrorMessage)
	OnClose       func(err error)
}

// Client represents a WebSocket signaling client connection to mezon-sfu.
type Client struct {
	wsURL    string
	handlers ClientHandlers

	conn      *websocket.Conn
	writeMu   sync.Mutex
	closed    atomic.Bool
	closeChan chan struct{}

	ctx    context.Context
	cancel context.CancelFunc

	lastActivity atomic.Int64 // Unix nanoseconds
}

// NewClient creates a new signaling client.
func NewClient(wsURL string, handlers ClientHandlers) *Client {
	return &Client{
		wsURL:     wsURL,
		handlers:  handlers,
		closeChan: make(chan struct{}),
	}
}

// Connect dials the WebSocket server and starts read/keepalive pumps.
func (c *Client) Connect(ctx context.Context) error {
	dialer := websocket.Dialer{
		HandshakeTimeout: 10 * time.Second,
	}

	header := http.Header{}
	header.Set("User-Agent", "mezon-sfu-loadtest/1.0")

	conn, resp, err := dialer.DialContext(ctx, c.wsURL, header)
	if err != nil {
		if resp != nil {
			return fmt.Errorf("websocket dial failed (status %d): %w", resp.StatusCode, err)
		}
		return fmt.Errorf("websocket dial failed: %w", err)
	}

	c.conn = conn
	c.ctx, c.cancel = context.WithCancel(ctx)
	c.markActivity()

	// Configure WS control frame ping/pong
	conn.SetPingHandler(func(appData string) error {
		c.markActivity()
		c.writeMu.Lock()
		defer c.writeMu.Unlock()
		return conn.WriteControl(websocket.PongMessage, []byte(appData), time.Now().Add(5*time.Second))
	})

	conn.SetPongHandler(func(appData string) error {
		c.markActivity()
		return nil
	})

	go c.readPump()
	go c.keepalivePump()

	return nil
}

func (c *Client) markActivity() {
	c.lastActivity.Store(time.Now().UnixNano())
}

// SendJSON sends a JSON message over the WebSocket connection.
func (c *Client) SendJSON(v interface{}) error {
	if c.closed.Load() {
		return ErrClientClosed
	}

	data, err := json.Marshal(v)
	if err != nil {
		return fmt.Errorf("failed to marshal message: %w", err)
	}

	c.writeMu.Lock()
	defer c.writeMu.Unlock()

	if c.conn == nil || c.closed.Load() {
		return ErrClientClosed
	}

	_ = c.conn.SetWriteDeadline(time.Now().Add(10 * time.Second))
	err = c.conn.WriteMessage(websocket.TextMessage, data)
	if err == nil {
		c.markActivity()
	}
	return err
}

// Join sends a join message with the provided JWT token and role.
func (c *Client) Join(token, role, screenCodec string) error {
	if role == "" {
		role = "speaker"
	}
	if screenCodec == "" {
		screenCodec = "vp8"
	}
	return c.SendJSON(JoinMessage{
		Type:        TypeJoin,
		Token:       token,
		Role:        role,
		ScreenCodec: screenCodec,
	})
}

// SendAnswer sends an answer SDP in response to a server offer.
func (c *Client) SendAnswer(sdp string, offerGeneration uint64) error {
	return c.SendJSON(AnswerMessage{
		Type:            TypeAnswer,
		SDP:             sdp,
		OfferGeneration: offerGeneration,
	})
}

// SendCamera sends a camera toggle message.
func (c *Client) SendCamera(active bool) error {
	return c.SendJSON(CameraMessage{
		Type:   TypeCamera,
		Active: active,
	})
}

// SendPing sends an explicit JSON ping message to the SFU signaling server.
func (c *Client) SendPing() error {
	return c.SendJSON(map[string]string{
		"type": string(TypePing),
	})
}

// SendPong responds to an SFU JSON ping.
func (c *Client) SendPong() error {
	return c.SendJSON(map[string]string{
		"type": string(TypePong),
	})
}

func (c *Client) readPump() {
	var closeErr error
	defer func() {
		c.Close()
		if c.handlers.OnClose != nil {
			c.handlers.OnClose(closeErr)
		}
	}()

	for {
		if c.closed.Load() {
			return
		}

		_, data, err := c.conn.ReadMessage()
		if err != nil {
			closeErr = err
			return
		}

		c.markActivity()
		var raw RawMessage
		if err := json.Unmarshal(data, &raw); err != nil {
			continue
		}

		switch raw.Type {
		case TypePing:
			// Server sent a JSON ping; respond with JSON pong immediately.
			_ = c.SendPong()

		case TypePong:
			// Server sent a JSON pong; keepalive recorded.

		case TypeJoined:
			var msg JoinedMessage
			if err := json.Unmarshal(data, &msg); err == nil && c.handlers.OnJoined != nil {
				c.handlers.OnJoined(msg)
			}

		case TypeOffer:
			var msg OfferMessage
			if err := json.Unmarshal(data, &msg); err == nil && c.handlers.OnOffer != nil {
				c.handlers.OnOffer(msg)
			}

		case TypePeerUpdated:
			var msg PeerUpdatedMessage
			if err := json.Unmarshal(data, &msg); err == nil && c.handlers.OnPeerUpdated != nil {
				c.handlers.OnPeerUpdated(msg)
			}

		case TypeError:
			var msg ErrorMessage
			if err := json.Unmarshal(data, &msg); err == nil && c.handlers.OnError != nil {
				c.handlers.OnError(msg)
			}
		}
	}
}

func (c *Client) keepalivePump() {
	ticker := time.NewTicker(5 * time.Second)
	defer ticker.Stop()

	for {
		select {
		case <-c.closeChan:
			return
		case <-c.ctx.Done():
			return
		case <-ticker.C:
			if c.closed.Load() {
				return
			}
			// Send periodic JSON ping to stay well within the 20s idle timeout
			if err := c.SendPing(); err != nil {
				return
			}
		}
	}
}

// Close gracefully closes the WebSocket connection.
func (c *Client) Close() error {
	if c.closed.CompareAndSwap(false, true) {
		close(c.closeChan)
		if c.cancel != nil {
			c.cancel()
		}

		c.writeMu.Lock()
		defer c.writeMu.Unlock()

		if c.conn != nil {
			_ = c.conn.WriteControl(
				websocket.CloseMessage,
				websocket.FormatCloseMessage(websocket.CloseNormalClosure, "leaving"),
				time.Now().Add(time.Second),
			)
			return c.conn.Close()
		}
	}
	return nil
}
