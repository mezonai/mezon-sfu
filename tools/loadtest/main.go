package main

import (
	"context"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"syscall"
	"time"

	"mezon-sfu/tools/loadtest/pkg/report"
	"mezon-sfu/tools/loadtest/pkg/runner"
)

func getEnvOrDefault(envKey, defaultValue string) string {
	if val, ok := os.LookupEnv(envKey); ok && val != "" {
		return val
	}
	return defaultValue
}

func main() {
	var (
		wsURL        = flag.String("url", getEnvOrDefault("SFU_WS_URL", "ws://127.0.0.1:8000/ws"), "Signaling WebSocket URL")
		jwtSecret    = flag.String("jwt-secret", getEnvOrDefault("SFU_JWT_SECRET", "secret"), "HMAC-SHA256 JWT Secret")
		rooms        = flag.Int("rooms", 30, "Number of concurrent rooms")
		peersPerRoom = flag.Int("peers", 10, "Peers per room")
		speakers     = flag.Int("speakers", 2, "Publishing speakers per room (remainder are audience)")
		duration     = flag.Duration("duration", 30*time.Second, "Test run duration after ramp-up")
		rampDuration = flag.Duration("ramp-duration", 5*time.Second, "Ramp-up duration to stagger peer connections")
		roomStartID  = flag.Uint64("room-start-id", 1000, "Initial room ID")
		userStartID  = flag.Int64("user-start-id", 10000, "Initial user ID")
		fps          = flag.Int("fps", 30, "Video target FPS for speakers")
		bitrateBps   = flag.Int("bitrate", 240000, "Video target bitrate in bps for speakers")

		// Thresholds
		minSuccessRate   = flag.Float64("min-success-rate", 95.0, "Minimum required peer connection success rate %")
		maxP95JoinTimeMs = flag.Float64("max-p95-join-time", 5000.0, "Maximum allowed p95 join latency in ms")
		maxPacketLossPct = flag.Float64("max-packet-loss", 5.0, "Maximum allowed average packet loss %")
		minRxPackets     = flag.Uint64("min-rx-packets", 0, "Minimum required total received packets")
		failOnThresholds = flag.Bool("fail-on-thresholds", true, "Exit with non-zero status code if thresholds fail")

		// Reporting
		reportFormat = flag.String("report-format", "both", "Report output format (text, json, both)")
		jsonFilePath = flag.String("json-file", "", "Optional file path to save the JSON report")
	)

	flag.Parse()

	// Handle OS interrupt / termination signals
	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()

	thresholds := report.Thresholds{
		MinSuccessRatePct: *minSuccessRate,
		MaxP95JoinTimeMs:  *maxP95JoinTimeMs,
		MaxPacketLossPct:  *maxPacketLossPct,
		MinRxPackets:      *minRxPackets,
	}

	cfg := runner.Config{
		WSURL:           *wsURL,
		JWTSecret:       *jwtSecret,
		Rooms:           *rooms,
		PeersPerRoom:    *peersPerRoom,
		SpeakersPerRoom: *speakers,
		RoomStartID:     *roomStartID,
		UserStartID:     *userStartID,
		Duration:        *duration,
		RampDuration:    *rampDuration,
		FPS:             *fps,
		BitrateBps:      *bitrateBps,
		Thresholds:      thresholds,
	}

	fmt.Printf("Starting Mezon SFU Load Generator...\n")
	fmt.Printf("Target: %s | %d Rooms x %d Peers (%d Speakers, %d Audience)\n",
		cfg.WSURL, cfg.Rooms, cfg.PeersPerRoom, cfg.SpeakersPerRoom, cfg.PeersPerRoom-cfg.SpeakersPerRoom)
	fmt.Printf("Duration: %s | Ramp-up: %s\n\n", cfg.Duration, cfg.RampDuration)

	r := runner.NewRunner(cfg)
	rep, err := r.Run(ctx)
	if err != nil && err != context.Canceled {
		fmt.Fprintf(os.Stderr, "Load test run failed: %v\n", err)
		os.Exit(1)
	}

	// Output text report
	if *reportFormat == "text" || *reportFormat == "both" {
		fmt.Print(rep.FormatReadable())
	}

	// Output JSON report
	if *reportFormat == "json" {
		if err := rep.WriteJSON(os.Stdout); err != nil {
			fmt.Fprintf(os.Stderr, "Failed to write JSON report to stdout: %v\n", err)
		}
	}

	// Save to JSON file if configured
	if *jsonFilePath != "" {
		if err := rep.SaveJSONFile(*jsonFilePath); err != nil {
			fmt.Fprintf(os.Stderr, "Failed to save JSON report to %s: %v\n", *jsonFilePath, err)
		} else {
			fmt.Printf("JSON report saved to %s\n", *jsonFilePath)
		}
	}

	if *failOnThresholds && !rep.Passed {
		os.Exit(1)
	}
}
