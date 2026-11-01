/*
 * AIO Feeder — feed_id journal (SR6, PLAN.md §6.6).
 *
 * The feed_id is persisted (IN_PROGRESS) *before* the relay turns on; the
 * result is recorded afterwards. Duplicates return the stored result and
 * never actuate again. On boot, an IN_PROGRESS entry means the feed was
 * already attempted — it is never repeated automatically.
 *
 * Storage: the runtime copies entries[] + head into the config blob; this
 * class only manipulates the array in RAM.
 */
#ifndef AIO_FEED_JOURNAL_H
#define AIO_FEED_JOURNAL_H

#include <stdint.h>
#include <string.h>
#include "proto.h"

#ifndef AIO_JOURNAL_SIZE
#define AIO_JOURNAL_SIZE 16
#endif

namespace aio {

struct JournalEntry {
  uint32_t feed_id = 0;
  uint16_t actual_on_ms = 0;
  uint8_t status = 0;         /* feed_result_t */
  uint8_t in_progress = 0;
} __attribute__((packed));

class FeedJournal {
 public:
  /* Dedup lookup: true if feed_id was seen; *result gets the stored outcome. */
  bool known(uint32_t feed_id, uint8_t *result_status) const {
    const JournalEntry *e = find(feed_id);
    if (!e) return false;
    if (result_status) *result_status = e->in_progress ? (uint8_t)FEED_ABORTED : e->status;
    return true;
  }

  /* Full entry lookup (runtime uses in_progress to re-answer a retried
   * FEED with FEED_ACK instead of a finished result, §6.6). */
  const JournalEntry *find(uint32_t feed_id) const {
    if (feed_id == 0) return nullptr;
    for (uint32_t i = 0; i < AIO_JOURNAL_SIZE; i++)
      if (entries_[i].feed_id == feed_id) return &entries_[i];
    return nullptr;
  }

  /* Write-before-actuate: mark IN_PROGRESS (caller persists the blob NOW). */
  void record_intent(uint32_t feed_id) { put(feed_id, 0, 0, 1); }

  /* Record the outcome once the actuation finished. */
  void record_result(uint32_t feed_id, uint8_t status, uint16_t actual_on_ms) {
    put(feed_id, status, actual_on_ms, 0);
  }

  /* Boot scan: IN_PROGRESS feeds were attempted before a reset. */
  uint32_t resolve_in_progress() {
    uint32_t n = 0;
    for (uint32_t i = 0; i < AIO_JOURNAL_SIZE; i++) {
      JournalEntry &e = entries_[i];
      if (e.feed_id != 0 && e.in_progress) {
        e.in_progress = 0;
        e.status = FEED_ABORTED;
        e.actual_on_ms = 0;  /* unknown partial; hub reconciles by feed_id */
        n++;
      }
    }
    return n;
  }

  const JournalEntry *entries() const { return entries_; }
  bool dirty = false;  /* set on every mutation; runtime persists when set */

 private:
  void put(uint32_t feed_id, uint8_t status, uint16_t actual_on_ms, uint8_t in_progress) {
    if (feed_id == 0) return;
    for (uint32_t i = 0; i < AIO_JOURNAL_SIZE; i++) {
      JournalEntry &e = entries_[i];
      if (e.feed_id == feed_id) {  /* update in place */
        e.status = status;
        e.actual_on_ms = actual_on_ms;
        e.in_progress = in_progress;
        dirty = true;
        return;
      }
    }
    entries_[head_].feed_id = feed_id;  /* ring: overwrite the oldest */
    entries_[head_].status = status;
    entries_[head_].actual_on_ms = actual_on_ms;
    entries_[head_].in_progress = in_progress;
    head_ = (head_ + 1) % AIO_JOURNAL_SIZE;
    dirty = true;
  }

  JournalEntry entries_[AIO_JOURNAL_SIZE]{};
  uint32_t head_ = 0;
};

}  // namespace aio
#endif /* AIO_FEED_JOURNAL_H */
