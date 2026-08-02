// Minimal Go workload for the demo stack.
//
// This one is not injected and cannot be: the binary is built with
// CGO_ENABLED=0, so it is statically linked, has no PT_INTERP, and ld.so
// never runs for it. Setting LD_PRELOAD would be a silent no-op.
//
// It appears in procwatch.otel_procs anyway, because the agentd collector
// walks /proc under hostPID and reads PROCWATCH_SERVICE from this process's
// environment to label the rows. It burns a little CPU and allocates on a
// timer so those metrics show movement rather than a flat line.

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

const port = 8080

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

	log.Printf("%s listening on %d (metrics only, no trace injection)", service, port)
	log.Fatal(http.ListenAndServe(fmt.Sprintf(":%d", port), nil))
}
