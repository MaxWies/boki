#include "engine/flags.h"

ABSL_FLAG(int, gateway_conn_per_worker, 2, "");
ABSL_FLAG(int, sequencer_conn_per_worker, 2, "");
ABSL_FLAG(int, shared_log_conn_per_worker, 2, "");

ABSL_FLAG(int, io_uring_entries, 128, "");
ABSL_FLAG(int, io_uring_fd_slots, 128, "");
ABSL_FLAG(bool, io_uring_sqpoll, false, "");
ABSL_FLAG(int, io_uring_sq_thread_idle_ms, 1, "");
ABSL_FLAG(int, io_uring_cq_nr_wait, 1, "");
ABSL_FLAG(int, io_uring_cq_wait_timeout_us, 0, "");

ABSL_FLAG(bool, enable_monitor, false, "");
ABSL_FLAG(bool, func_worker_use_engine_socket, false, "");
ABSL_FLAG(bool, use_fifo_for_nested_call, false, "");
ABSL_FLAG(bool, func_worker_pipe_direct_write, false, "");

ABSL_FLAG(double, max_relative_queueing_delay, 0.0, "");
ABSL_FLAG(double, concurrency_limit_coef, 1.0, "");
ABSL_FLAG(double, expected_concurrency_coef, 1.0, "");
ABSL_FLAG(int, min_worker_request_interval_ms, 200, "");
ABSL_FLAG(bool, always_request_worker_if_possible, false, "");
ABSL_FLAG(bool, disable_concurrency_limiter, false, "");

ABSL_FLAG(double, instant_rps_p_norm, 1.0, "");
ABSL_FLAG(double, instant_rps_ema_alpha, 0.001, "");
ABSL_FLAG(double, instant_rps_ema_tau_ms, 0, "");

ABSL_FLAG(bool, enable_shared_log, false, "");
ABSL_FLAG(int, shared_log_num_replicas, 2, "");
ABSL_FLAG(int, shared_log_local_cut_interval_us, 1000, "");
ABSL_FLAG(int, shared_log_global_cut_interval_us, 1000, "");
