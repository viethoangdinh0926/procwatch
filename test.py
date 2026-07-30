import multiprocessing as mp
import os
import random
import threading
import time


def cpu_mem_churn(label: str):
    """Spin CPU and occasionally allocate memory forever."""
    stash = []
    print(f"{label} started (pid={os.getpid()})")
    while True:
        # Busy work
        _ = sum(i * i for i in range(10_000))
        # Occasional allocations to grow RSS
        if random.random() < 0.2:
            block = bytearray(random.randint(50_000, 200_000))
            stash.append(block)
        # Bound growth pace a bit
        time.sleep(0.01)


def main():
    print("Root PID:", os.getpid())

    threads = []
    for i in range(2):
        t = threading.Thread(target=cpu_mem_churn, args=(f"thread-{i}",), daemon=True)
        t.start()
        threads.append(t)

    procs = []
    for i in range(10):
        time.sleep(random.uniform(10.0, 15.0))
        p = mp.Process(target=cpu_mem_churn, args=(f"proc-{i}",))
        p.start()
        procs.append(p)

    try:
        # Keep the main process alive
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        for p in procs:
            p.terminate()
        for p in procs:
            p.join()


if __name__ == "__main__":
    main()