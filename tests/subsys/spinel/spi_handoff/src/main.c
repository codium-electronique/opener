/*
 * Copyright (c) 2026 Codium Electronique
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/ztest.h>

#include "spi_handoff.h"
#include "spi_link.h"

#define BUF_SIZE     700
#define RECV_LEN     1300
#define STRESS_SENDS 2000
#define STRESS_STACK 2048
#define POISON       0xEE
#define PAYLOAD      0x11

static struct spinel_spi_handoff handoff;
static struct spinel_spi_link link;
static uint8_t tx_buf[BUF_SIZE];

/* Runs in the sender's context, from the wake hook. */
static void (*wake_action)(void);

static uint32_t armed_seq;
static size_t armed_total;
static bool done_result;
static bool copy_ok;
static const uint8_t *expected_frame;
static size_t expected_len;
static uint32_t stale_seq;
static bool stale_done;

static void wake_hook(void)
{
	if (wake_action != NULL) {
		wake_action();
	}
}

static void arm_now(void)
{
	size_t total = 0;
	size_t frame_len = 0;
	uint32_t seq = 0;

	(void)spinel_spi_handoff_arm(&handoff, &link, true, tx_buf, sizeof(tx_buf), &total, &seq,
				     &frame_len);
	armed_total = total;
	armed_seq = seq;
}

static void arm_check_and_done(void)
{
	int cmp;

	arm_now();
	cmp = memcmp(&tx_buf[SPINEL_SPI_HEADER_LEN], expected_frame, expected_len);
	copy_ok = (cmp == 0) && (armed_total == SPINEL_SPI_HEADER_LEN + expected_len);
	done_result = spinel_spi_handoff_done(&handoff, armed_seq);
}

/* Completes an earlier frame once the next one is pending, as a late transaction would. */
static void done_stale(void)
{
	stale_done = spinel_spi_handoff_done(&handoff, stale_seq);
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	wake_action = NULL;
	armed_seq = 0;
	armed_total = 0;
	done_result = false;
	copy_ok = false;
	stale_seq = 0;
	stale_done = false;
	memset(tx_buf, 0, sizeof(tx_buf));
	spinel_spi_link_init(&link, RECV_LEN);
	spinel_spi_handoff_init(&handoff, wake_hook);
}

ZTEST(spinel_spi_handoff, test_send_returns_once_done)
{
	static const uint8_t frame[] = {1, 2, 3, 4, 5, 6, 7};
	int ret;

	expected_frame = frame;
	expected_len = sizeof(frame);
	wake_action = arm_check_and_done;

	ret = spinel_spi_handoff_send(&handoff, frame, sizeof(frame), K_MSEC(100));

	zassert_equal(ret, 0);
	zassert_true(copy_ok);
	zassert_true(done_result);
}

ZTEST(spinel_spi_handoff, test_timeout_drops_frame)
{
	static const uint8_t frame[] = {1, 2, 3};
	size_t total = 0;
	size_t frame_len = 0;
	uint32_t seq = 0;
	int ret;

	ret = spinel_spi_handoff_send(&handoff, frame, sizeof(frame), K_MSEC(5));
	zassert_equal(ret, -ETIMEDOUT);

	ret = spinel_spi_handoff_arm(&handoff, &link, true, tx_buf, sizeof(tx_buf), &total, &seq,
				     &frame_len);
	zassert_equal(ret, 0);
	zassert_equal(frame_len, 0);
	zassert_equal(link.armed_len, 0);
	zassert_equal(total, SPINEL_SPI_HEADER_LEN);
}

ZTEST(spinel_spi_handoff, test_late_done_does_not_complete_next)
{
	static const uint8_t frame[] = {9, 8, 7};
	uint32_t seq_a;
	bool late;
	int ret;

	wake_action = arm_now;
	ret = spinel_spi_handoff_send(&handoff, frame, sizeof(frame), K_MSEC(5));
	zassert_equal(ret, -ETIMEDOUT);
	seq_a = armed_seq;

	late = spinel_spi_handoff_done(&handoff, seq_a);
	zassert_false(late);

	wake_action = NULL;
	ret = spinel_spi_handoff_send(&handoff, frame, sizeof(frame), K_MSEC(5));
	zassert_equal(ret, -ETIMEDOUT);
}

ZTEST(spinel_spi_handoff, test_stale_done_while_next_pending)
{
	static const uint8_t frame_a[] = {9, 8, 7};
	static const uint8_t frame_b[] = {6, 5, 4};
	int ret;

	/* Frame A is armed, then its sender times out. */
	wake_action = arm_now;
	ret = spinel_spi_handoff_send(&handoff, frame_a, sizeof(frame_a), K_MSEC(5));
	zassert_equal(ret, -ETIMEDOUT);
	stale_seq = armed_seq;

	/* The transaction of A completes while B is pending: B must stay pending. */
	wake_action = done_stale;
	ret = spinel_spi_handoff_send(&handoff, frame_b, sizeof(frame_b), K_MSEC(5));
	zassert_false(stale_done, "the completion of A completed B");
	zassert_equal(ret, -ETIMEDOUT);
}

ZTEST(spinel_spi_handoff, test_frame_too_long)
{
	static uint8_t frame[BUF_SIZE];
	size_t len = BUF_SIZE - SPINEL_SPI_HEADER_LEN + 1;
	size_t total = 0;
	size_t frame_len = 0;
	uint32_t seq = 0;
	int ret;

	memset(frame, PAYLOAD, sizeof(frame));

	/* Publish the frame by hand: no sender is waiting for it. */
	k_mutex_lock(&handoff.lock, K_FOREVER);
	handoff.frame = frame;
	handoff.len = len;
	k_mutex_unlock(&handoff.lock);

	ret = spinel_spi_handoff_arm(&handoff, &link, true, tx_buf, sizeof(tx_buf), &total, &seq,
				     &frame_len);
	zassert_equal(ret, -EMSGSIZE);
	zassert_equal(frame_len, len);
	zassert_equal(link.armed_len, 0);
	zassert_equal(total, SPINEL_SPI_HEADER_LEN);
}

/* Stress: an SPI thread arms and completes while senders run at another priority. */

static struct k_thread spi_thread;
static K_THREAD_STACK_DEFINE(spi_stack, STRESS_STACK);
static struct k_thread sender_thread;
static K_THREAD_STACK_DEFINE(sender_stack, STRESS_STACK);

static volatile bool stop;
/* Whether the copy window lets other threads run, only while a stress test runs. */
static volatile bool copy_window_open;
static uint32_t completions;
static uint32_t poisoned;
static uint32_t sends_ok;
static uint8_t sender_buf[64];
static struct k_sem sender_finished;

/*
 * native_sim only switches threads at kernel calls: without this the copy of a frame has no
 * preemption point, and a copy moved outside the lock would go unnoticed. A busy wait lets a
 * higher priority sender time out and run, a sleep lets a lower priority one run.
 */
void spinel_spi_handoff_test_copy_window(void)
{
	uint32_t wait_us;
	uint32_t choice;

	if (!copy_window_open) {
		return;
	}

	wait_us = sys_rand32_get() % 50;
	choice = sys_rand32_get() % 2;
	if (choice == 0) {
		k_busy_wait(wait_us);
	} else {
		k_usleep(wait_us);
	}
}

static void stress_spi_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	static uint8_t spi_tx[BUF_SIZE];

	while (!stop) {
		size_t total = 0;
		size_t frame_len = 0;
		uint32_t seq = 0;
		uint32_t wait_us;
		bool done;

		(void)spinel_spi_handoff_arm(&handoff, &link, true, spi_tx, sizeof(spi_tx), &total,
					     &seq, &frame_len);
		for (size_t i = SPINEL_SPI_HEADER_LEN; i < total; i++) {
			if (spi_tx[i] == POISON) {
				poisoned++;
				break;
			}
		}
		wait_us = sys_rand32_get() % 50;
		k_busy_wait(wait_us);
		done = spinel_spi_handoff_done(&handoff, seq);
		if (done) {
			completions++;
		}
		k_usleep(5);
	}
}

static void stress_sender_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (int i = 0; i < STRESS_SENDS; i++) {
		uint32_t len = 1 + (sys_rand32_get() % 64);
		uint32_t timeout_us = sys_rand32_get() % 100;
		int ret;

		memset(sender_buf, PAYLOAD, len);
		ret = spinel_spi_handoff_send(&handoff, sender_buf, len, K_USEC(timeout_us));
		memset(sender_buf, POISON, sizeof(sender_buf));
		if (ret == 0) {
			sends_ok++;
		}
	}
	k_sem_give(&sender_finished);
}

static void run_stress(int spi_prio, int sender_prio)
{
	stop = false;
	copy_window_open = true;
	completions = 0;
	poisoned = 0;
	sends_ok = 0;
	k_sem_init(&sender_finished, 0, 1);

	k_thread_create(&spi_thread, spi_stack, K_THREAD_STACK_SIZEOF(spi_stack), stress_spi_fn,
			NULL, NULL, NULL, spi_prio, 0, K_NO_WAIT);
	k_thread_create(&sender_thread, sender_stack, K_THREAD_STACK_SIZEOF(sender_stack),
			stress_sender_fn, NULL, NULL, NULL, sender_prio, 0, K_NO_WAIT);

	int ret = k_sem_take(&sender_finished, K_SECONDS(60));

	stop = true;
	copy_window_open = false;
	k_thread_join(&spi_thread, K_SECONDS(5));
	k_thread_join(&sender_thread, K_SECONDS(5));

	zassert_equal(ret, 0, "stress sender did not finish");
	zassert_equal(poisoned, 0, "a frame was armed after its sender returned");
	zassert_equal(sends_ok, completions, "sends %u, completions %u", sends_ok, completions);
}

ZTEST(spinel_spi_handoff, test_stress_sender_preempts)
{
	run_stress(K_PRIO_PREEMPT(2), K_PRIO_PREEMPT(1));
}

ZTEST(spinel_spi_handoff, test_stress_spi_preempts)
{
	run_stress(K_PRIO_PREEMPT(1), K_PRIO_PREEMPT(2));
}

ZTEST_SUITE(spinel_spi_handoff, NULL, NULL, before, NULL, NULL);
