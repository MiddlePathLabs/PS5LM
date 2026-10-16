/* SPDX-License-Identifier: GPL-3.0-or-later
 * Server mode (build modes "server" and "server-qwen"): llama.cpp's server in
 * the title, for a bounded serving window. The model, its file check and the
 * weights/context/total caps are the model mode's (ggml-model.h); the server
 * settings and the lifetime are here. docs/PORTING.md "LAN API".
 */
#ifndef PS5LM_GGML_SERVER_H
#define PS5LM_GGML_SERVER_H
#if defined(PS5LM_MODEL_MOE)
#define PS5LM_SERVER_WORKLOAD "server-moe"
#else
#define PS5LM_SERVER_WORKLOAD "server-dense"
#endif
#define PS5LM_SERVER_PORT "8081"
#ifndef PS5LM_SERVER_HOST
#define PS5LM_SERVER_HOST "0.0.0.0" /* the console's one LAN interface; only while the window is open */
#endif
/* One slot, a 2048-token context (the 512-token benchmark prompt plus 128
 * generated tokens fit with room to spare), and the model mode's batches. */
#define PS5LM_SERVER_CTX 2048
#define PS5LM_SERVER_BATCH 32
#define PS5LM_SERVER_HTTP_THREADS "4"
/* Lifetime, seconds. The model must be loaded within LOAD; the server then
 * serves for WINDOW or until POST /ps5lm/shutdown, whichever comes first, and
 * must have stopped and freed within STOP. The watchdog covers all of it. */
#define PS5LM_SERVER_LOAD_S 120u
#ifndef PS5LM_SERVER_WINDOW_S /* the host rehearsal shortens it */
#define PS5LM_SERVER_WINDOW_S 600u
#endif
#define PS5LM_SERVER_STOP_S 60u
#define PS5LM_SERVER_WATCHDOG_S (PS5LM_SERVER_LOAD_S + PS5LM_SERVER_WINDOW_S + PS5LM_SERVER_STOP_S + 60u)
/* The server's own thread; the platform's default thread stack is small. */
#define PS5LM_SERVER_STACK_BYTES (16u << 20)
/* Queue submissions over the whole launch. The model mode's 8192 covers one
 * load and 32 tokens; a serving window decodes thousands of tokens at about
 * 13 submissions each. Still a hard cap, not a liveness rule: the window is. */
#define PS5LM_SERVER_SUBMIT_LIMIT (1u << 20)
#endif
