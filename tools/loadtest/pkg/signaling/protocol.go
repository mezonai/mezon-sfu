package signaling

import (
	"encoding/json"
)

// MessageType indicates the type of signaling message.
type MessageType string

const (
	TypeJoin        MessageType = "join"
	TypeJoined      MessageType = "joined"
	TypeOffer       MessageType = "offer"
	TypeAnswer      MessageType = "answer"
	TypePing        MessageType = "ping"
	TypePong        MessageType = "pong"
	TypeCamera      MessageType = "camera"
	TypeMute        MessageType = "mute"
	TypePeerJoined  MessageType = "peer_joined"
	TypePeerLeft    MessageType = "peer_left"
	TypePeerUpdated MessageType = "peer_updated"
	TypeError       MessageType = "error"
	TypeMuteChanged MessageType = "mute_changed"
	TypePushToTalk  MessageType = "push_to_talk"
	TypePTTChanged  MessageType = "push_to_talk_changed"
)

// RawMessage represents a generic signaling envelope to inspect the type.
type RawMessage struct {
	Type MessageType `json:"type"`
}

// JoinMessage is sent by the client to authenticate and join a room.
type JoinMessage struct {
	Type        MessageType `json:"type"`
	Token       string      `json:"token"`
	Role        string      `json:"role,omitempty"`         // "speaker" or "audience"
	ScreenCodec string      `json:"screen_codec,omitempty"` // "vp8" or "vp9"
}

// JoinedMessage is returned by the server upon successful join.
type JoinedMessage struct {
	Type       MessageType `json:"type"`
	Room       string      `json:"room"`
	ICEServers []ICEServer `json:"iceServers,omitempty"`
}

// ICEServer represents a STUN/TURN server configuration.
type ICEServer struct {
	URLs       interface{} `json:"urls"` // string or []string
	Username   string      `json:"username,omitempty"`
	Credential string      `json:"credential,omitempty"`
}

// OfferMessage represents a server-initiated SDP offer.
type OfferMessage struct {
	Type            MessageType `json:"type"`
	OfferGeneration uint64      `json:"offer_generation"`
	SDP             string      `json:"sdp"`
}

// AnswerMessage is sent by the client in response to a server offer.
type AnswerMessage struct {
	Type            MessageType `json:"type"`
	SDP             string      `json:"sdp"`
	OfferGeneration uint64      `json:"offer_generation"`
}

// CameraMessage enables or disables camera state on the SFU.
type CameraMessage struct {
	Type   MessageType `json:"type"`
	Active bool        `json:"active"`
}

// MuteMessage toggles audio mute state.
type MuteMessage struct {
	Type   MessageType `json:"type"`
	IsMute bool        `json:"is_mute"`
}

// ErrorMessage represents an error notification from the server.
type ErrorMessage struct {
	Type    MessageType `json:"type"`
	Message string      `json:"message"`
}

// PeerMember holds metadata about a room member.
type PeerMember struct {
	PeerID          uint32  `json:"peer_id"`
	UserID          string  `json:"user_id"`
	Role            string  `json:"role"`
	IsMute          bool    `json:"is_mute"`
	CameraRequested bool    `json:"camera_requested"`
	CameraActive    bool    `json:"camera_active"`
	ScreenRequested bool    `json:"screen_requested"`
	ScreenActive    bool    `json:"screen_active"`
	RemoteSlot      *uint32 `json:"remote_slot,omitempty"`
}

// PeerUpdatedMessage is broadcast when a peer's state changes.
type PeerUpdatedMessage struct {
	Type MessageType `json:"type"`
	Peer PeerMember  `json:"peer"`
}

// EncodeJSON returns JSON bytes for any signaling message.
func EncodeJSON(v interface{}) ([]byte, error) {
	return json.Marshal(v)
}
