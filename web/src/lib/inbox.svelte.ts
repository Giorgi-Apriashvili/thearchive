// Links members have sent you: the unread count behind the Uploads tab's badge, and the
// items the "Shared with you" list shows.
//
// Only the count is polled — every ten seconds for as long as you are signed in, paused
// while the tab is hidden, like the chat room list. The items themselves load when the
// list is actually on screen, which is also when they are marked seen.

import { api, ApiError, type InboxItem } from './api'

const POLL_MS = 10_000

class InboxStore {
  unread = $state(0)
  items = $state<InboxItem[] | null>(null)
  error = $state('')

  #timer: ReturnType<typeof setInterval> | null = null
  #inFlight = false

  watch(): () => void {
    void this.#poll()
    this.#timer = setInterval(() => void this.#poll(), POLL_MS)
    document.addEventListener('visibilitychange', this.#onVisibility)
    return () => {
      if (this.#timer !== null) clearInterval(this.#timer)
      this.#timer = null
      document.removeEventListener('visibilitychange', this.#onVisibility)
    }
  }

  #onVisibility = () => {
    if (document.visibilityState === 'visible') void this.#poll()
  }

  async #poll() {
    if (document.visibilityState === 'hidden' || this.#inFlight) return
    this.#inFlight = true
    try {
      this.unread = (await api.get<{ count: number }>('/api/inbox/unread')).count
    } catch {
      // A missed poll changes nothing; the next one will try again.
    } finally {
      this.#inFlight = false
    }
  }

  async load() {
    try {
      this.items = (await api.get<{ items: InboxItem[] }>('/api/inbox')).items
      this.error = ''
    } catch (e) {
      this.error = e instanceof ApiError ? e.message : 'links sent to you could not be loaded'
    }
  }

  /** Clears the badge. The loaded items keep their `seen` flags as they were, so what
   *  arrived since you last looked stays marked as new for as long as you are looking. */
  async markSeen() {
    if (this.unread === 0) return
    try {
      await api.post('/api/inbox/seen')
      this.unread = 0
    } catch {
      // The badge simply stays until the next attempt.
    }
  }

  async dismiss(id: string) {
    try {
      await api.del(`/api/inbox/items/${id}`)
      this.items = (this.items ?? []).filter((item) => item.id !== id)
    } catch (e) {
      this.error = e instanceof ApiError ? e.message : 'could not dismiss that'
    }
  }
}

export const inbox = new InboxStore()
