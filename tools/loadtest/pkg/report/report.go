package report

import (
	"encoding/json"
	"fmt"
	"io"
	"os"
	"strings"
	"time"

	"mezon-sfu/tools/loadtest/pkg/metrics"
)

// Thresholds configures performance criteria for the load test run.
type Thresholds struct {
	MinSuccessRatePct float64 `json:"min_success_rate_pct"`
	MaxP95JoinTimeMs  float64 `json:"max_p95_join_time_ms"`
	MaxPacketLossPct  float64 `json:"max_packet_loss_pct"`
	MinRxPackets      uint64  `json:"min_rx_packets"`
}

// ThresholdResult represents the evaluation of a single threshold criterion.
type ThresholdResult struct {
	Name     string `json:"name"`
	Target   string `json:"target"`
	Actual   string `json:"actual"`
	Passed   bool   `json:"passed"`
	ErrorMsg string `json:"error_msg,omitempty"`
}

// TestConfig contains the configuration of the load test run.
type TestConfig struct {
	WSURL        string        `json:"ws_url"`
	Rooms        int           `json:"rooms"`
	PeersPerRoom int           `json:"peers_per_room"`
	Speakers     int           `json:"speakers_per_room"`
	Duration     time.Duration `json:"duration"`
	RampDuration time.Duration `json:"ramp_duration"`
	FPS          int           `json:"fps"`
	BitrateBps   int           `json:"bitrate_bps"`
}

// TestReport contains the full report data for JSON and human output.
type TestReport struct {
	Timestamp        time.Time                 `json:"timestamp"`
	Config           TestConfig                `json:"config"`
	Summary          metrics.AggregatedSummary `json:"summary"`
	Thresholds       Thresholds                `json:"thresholds"`
	ThresholdResults []ThresholdResult         `json:"threshold_results"`
	Passed           bool                      `json:"passed"`
}

// EvaluateThresholds checks metrics against the defined thresholds.
func EvaluateThresholds(summary metrics.AggregatedSummary, th Thresholds) ([]ThresholdResult, bool) {
	var results []ThresholdResult
	allPassed := true

	// 1. Success Rate
	if th.MinSuccessRatePct > 0 {
		passed := summary.SuccessRatePct >= th.MinSuccessRatePct
		if !passed {
			allPassed = false
		}
		results = append(results, ThresholdResult{
			Name:   "Peer Connection Success Rate",
			Target: fmt.Sprintf(">= %.1f%%", th.MinSuccessRatePct),
			Actual: fmt.Sprintf("%.2f%%", summary.SuccessRatePct),
			Passed: passed,
		})
	}

	// 2. P95 Join Latency
	if th.MaxP95JoinTimeMs > 0 {
		passed := summary.JoinLatency.P95Ms <= th.MaxP95JoinTimeMs
		if !passed {
			allPassed = false
		}
		results = append(results, ThresholdResult{
			Name:   "P95 Join Latency",
			Target: fmt.Sprintf("<= %.1f ms", th.MaxP95JoinTimeMs),
			Actual: fmt.Sprintf("%.2f ms", summary.JoinLatency.P95Ms),
			Passed: passed,
		})
	}

	// 3. Packet Loss %
	if th.MaxPacketLossPct > 0 {
		passed := summary.AvgLossRatePct <= th.MaxPacketLossPct
		if !passed {
			allPassed = false
		}
		results = append(results, ThresholdResult{
			Name:   "Average Packet Loss",
			Target: fmt.Sprintf("<= %.1f%%", th.MaxPacketLossPct),
			Actual: fmt.Sprintf("%.2f%%", summary.AvgLossRatePct),
			Passed: passed,
		})
	}

	// 4. Min RX Packets
	if th.MinRxPackets > 0 && summary.AudienceCount > 0 {
		totalRx := summary.TotalAudioPacketsRecv + summary.TotalVideoPacketsRecv
		passed := totalRx >= th.MinRxPackets
		if !passed {
			allPassed = false
		}
		results = append(results, ThresholdResult{
			Name:   "Total Received Packets",
			Target: fmt.Sprintf(">= %d pkts", th.MinRxPackets),
			Actual: fmt.Sprintf("%d pkts", totalRx),
			Passed: passed,
		})
	}

	return results, allPassed
}

// GenerateReport creates a structured TestReport object.
func GenerateReport(cfg TestConfig, summary metrics.AggregatedSummary, th Thresholds) TestReport {
	results, passed := EvaluateThresholds(summary, th)
	return TestReport{
		Timestamp:        time.Now(),
		Config:           cfg,
		Summary:          summary,
		Thresholds:       th,
		ThresholdResults: results,
		Passed:           passed,
	}
}

// WriteJSON writes the JSON report to the specified writer.
func (r *TestReport) WriteJSON(w io.Writer) error {
	enc := json.NewEncoder(w)
	enc.SetIndent("", "  ")
	return enc.Encode(r)
}

// SaveJSONFile writes the JSON report to a file on disk.
func (r *TestReport) SaveJSONFile(filePath string) error {
	f, err := os.Create(filePath)
	if err != nil {
		return err
	}
	defer f.Close()
	return r.WriteJSON(f)
}

// FormatReadable returns a human-friendly ASCII table report.
func (r *TestReport) FormatReadable() string {
	var sb strings.Builder
	sep := strings.Repeat("=", 78)
	subsep := strings.Repeat("-", 78)

	sb.WriteString("\n" + sep + "\n")
	sb.WriteString("                    MEZON SFU LOAD TEST REPORT\n")
	sb.WriteString(sep + "\n")

	sb.WriteString(fmt.Sprintf("Timestamp       : %s\n", r.Timestamp.Format(time.RFC3339)))
	sb.WriteString(fmt.Sprintf("Signaling Target: %s\n", r.Config.WSURL))
	sb.WriteString(fmt.Sprintf("Topology        : %d Rooms x %d Peers (%d Speakers, %d Audience)\n",
		r.Config.Rooms, r.Config.PeersPerRoom, r.Config.Speakers, r.Config.PeersPerRoom-r.Config.Speakers))
	sb.WriteString(fmt.Sprintf("Test Duration   : %s (Ramp-up: %s)\n", r.Config.Duration, r.Config.RampDuration))
	sb.WriteString(fmt.Sprintf("Media Config    : %d FPS, %d kbps\n", r.Config.FPS, r.Config.BitrateBps/1000))

	sb.WriteString("\n" + subsep + "\n")
	sb.WriteString("PEER CONNECTION SUMMARY\n")
	sb.WriteString(subsep + "\n")
	sb.WriteString(fmt.Sprintf("Total Rooms     : %-8d  Total Peers     : %d\n", r.Summary.TotalRooms, r.Summary.TotalPeers))
	sb.WriteString(fmt.Sprintf("Connected Peers : %-8d  Failed Peers    : %d\n", r.Summary.ConnectedPeers, r.Summary.FailedPeers))
	sb.WriteString(fmt.Sprintf("Success Rate    : %.2f%%\n", r.Summary.SuccessRatePct))
	sb.WriteString(fmt.Sprintf("Elapsed Time    : %.2f seconds\n", float64(r.Summary.DurationMs)/1000.0))

	sb.WriteString("\n" + subsep + "\n")
	sb.WriteString("JOIN LATENCY BREAKDOWN (ms)\n")
	sb.WriteString(subsep + "\n")
	sb.WriteString(fmt.Sprintf("Min: %8.2f ms | Mean: %8.2f ms | P50: %8.2f ms | P90: %8.2f ms\n",
		r.Summary.JoinLatency.MinMs, r.Summary.JoinLatency.MeanMs, r.Summary.JoinLatency.P50Ms, r.Summary.JoinLatency.P90Ms))
	sb.WriteString(fmt.Sprintf("P95: %8.2f ms | P99:  %8.2f ms | Max: %8.2f ms\n",
		r.Summary.JoinLatency.P95Ms, r.Summary.JoinLatency.P99Ms, r.Summary.JoinLatency.MaxMs))

	sb.WriteString("\n" + subsep + "\n")
	sb.WriteString("MEDIA TRANSMISSION & RECEPTION\n")
	sb.WriteString(subsep + "\n")
	sb.WriteString(fmt.Sprintf("TX Audio Packets: %-12d | RX Audio Packets: %-12d\n",
		r.Summary.TotalAudioPacketsSent, r.Summary.TotalAudioPacketsRecv))
	sb.WriteString(fmt.Sprintf("TX Video Packets: %-12d | RX Video Packets: %-12d\n",
		r.Summary.TotalVideoPacketsSent, r.Summary.TotalVideoPacketsRecv))
	sb.WriteString(fmt.Sprintf("TX Total Data   : %-9.2f MB  | RX Total Data   : %-9.2f MB\n",
		float64(r.Summary.TotalBytesSent)/(1024*1024), float64(r.Summary.TotalBytesRecv)/(1024*1024)))
	sb.WriteString(fmt.Sprintf("TX Throughput   : %-9.2f kbps| RX Throughput   : %-9.2f kbps\n",
		r.Summary.TotalTxBitrateKbps, r.Summary.TotalRxBitrateKbps))
	sb.WriteString(fmt.Sprintf("Packets Lost    : %-12d | Avg Loss Rate   : %.2f%%\n",
		r.Summary.TotalPacketsLost, r.Summary.AvgLossRatePct))

	sb.WriteString("\n" + subsep + "\n")
	sb.WriteString("THRESHOLD EVALUATION\n")
	sb.WriteString(subsep + "\n")
	for _, tr := range r.ThresholdResults {
		status := "[ PASS ]"
		if !tr.Passed {
			status = "[ FAIL ]"
		}
		sb.WriteString(fmt.Sprintf("%-32s Target: %-12s Actual: %-12s %s\n",
			tr.Name, tr.Target, tr.Actual, status))
	}

	sb.WriteString("\n" + sep + "\n")
	if r.Passed {
		sb.WriteString("OVERALL VERDICT: PASS\n")
	} else {
		sb.WriteString("OVERALL VERDICT: FAIL\n")
	}
	sb.WriteString(sep + "\n\n")

	return sb.String()
}
