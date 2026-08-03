// Minimal Go workload for the demo stack.
//
// This binary is built with CGO_ENABLED=0 (static, no PT_INTERP), so
// LD_PRELOAD is a silent no-op. Process metrics come from procwatch-wrap,
// which forks this process as a child and POSTs /v1/procmetrics under
// PROCWATCH_LABEL. No in-process inject thread is possible without ptrace.

package main

import (
	"encoding/json"
	"fmt"
	"log"
	"math/rand"
	"net/http"
	"os"
	"time"
)

func listenPort() string {
	if p := os.Getenv("PORT"); p != "" {
		return p
	}
	return "8080"
}

type stockResponse struct {
	SKU       string `json:"sku"`
	Available int    `json:"available"`
}

func main() {
	service := os.Getenv("PROCWATCH_SERVICE")
	if service == "" {
		service = "inventory"
	}

	http.HandleFunc("/stock", func(w http.ResponseWriter, r *http.Request) {
		time.Sleep(time.Duration(20+rand.Intn(60)) * time.Millisecond)
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(stockResponse{
			SKU:       "SKU-1234",
			Available: rand.Intn(100),
		})
	})
	http.HandleFunc("/health", func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, "ok")
	})

	// Varying CPU and heap so the collected metrics are not a flat line.
	go func() {
		for {
			ballast := make([]byte, 8<<20)
			for i := range ballast {
				ballast[i] = byte(i)
			}
			sum := 0
			deadline := time.Now().Add(300 * time.Millisecond)
			for time.Now().Before(deadline) {
				sum += rand.Intn(1000)
			}
			_ = sum
			_ = ballast
			time.Sleep(4 * time.Second)
		}
	}()

	port := listenPort()
	log.Printf("%s listening on %s (metrics only, no trace injection)", service, port)
	log.Fatal(http.ListenAndServe(":"+port, nil))
}
