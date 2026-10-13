// Polling state for the chat pane.
//
// Delivery is polling rather than SSE or websockets: a handful of friends talking to
// each other generates a request every couple of seconds against an indexed
// `WHERE room_id = ? AND id > ?`, which is close to free, and it keeps the server free
// of long-lived connections and the reverse proxy free of special cases. Nothing in the
// message API assumes it — `since` is a cursor, so moving to SSE later is additive.
//
// Two cadences, because they answer different questions. The room list carries the
// unread badges, so it keeps ticking slowly for as long as anyone is signed in —
// otherwise a new message would go unannounced until you happened to open the chat tab,
// which rather defeats the point. Messages poll quickly, and only while a room is open.

import { api, ApiError, type ChatMessage, type ChatRoom, type RoomMembers } from './api'
import { notify } from './notify.svelte'

const MESSAGE_MS = 2000
const ROOM_MS = 10000
/** Every Nth message poll also refreshes who is in the room. */
const MEMBERS_EVERY = 5

class ChatStore {
  rooms = $state<ChatRoom[]>([])
  loadingRooms = $state(true)
  roomsError = $state('')

  openId = $state<number | null>(null)
  messages = $state<ChatMessage[]>([])
  /** Whether there is history before the oldest message held — scrolling back loads it. */
  hasOlder = $state(false)
  loadingOlder = $state(false)
  /** Where I had read up to when the room was opened: the "New messages" line goes after
   *  it, and stays put while the room is open even though reading moves on. */
  lastReadAtOpen = $state(0)
  loadingMessages = $state(false)
  messagesError = $state('')
  /** Who is in the open room. Feeds the header's member list and the composer's @
   *  autocomplete, which is why it is here rather than local to a component. */
  members = $state<RoomMembers | null>(null)

  #roomTimer: ReturnType<typeof setInterval> | null = null
  #messageTimer: ReturnType<typeof setInterval> | null = null
  #polls = 0
  /** Highest message id seen, so a poll asks only for what it does not have. */
  #cursor = 0
  /** The server's clock at the last poll: edits and removals since then come back with
   *  the next one, so messages already on screen change without re-reading the room. */
  #changedSince = 0
  /** Highest id reported read, so an unchanged room does not re-POST it every tick. */
  #marked = 0
  #roomsInFlight = false
  #messagesInFlight = false

  get open(): ChatRoom | null {
    return this.rooms.find((r) => r.id === this.openId) ?? null
  }

  get totalUnread(): number {
    return this.rooms.reduce((sum, r) => sum + (r.unread ?? 0), 0)
  }

  get totalMentions(): number {
    return this.rooms.reduce((sum, r) => sum + (r.mentions_unread ?? 0), 0)
  }

  /** The slow poll: room list and unread badges. Started once by the signed-in shell
   *  and left running whichever tab is showing. Returns its teardown. */
  watch(): () => void {
    void this.#roomTick()
    this.#roomTimer = setInterval(() => void this.#roomTick(), ROOM_MS)
    document.addEventListener('visibilitychange', this.#onVisibility)
    return () => {
      if (this.#roomTimer !== null) clearInterval(this.#roomTimer)
      this.#roomTimer = null
      document.removeEventListener('visibilitychange', this.#onVisibility)
    }
  }

  /** The fast poll, for the open conversation. Started by the chat pane, so leaving it
   *  drops back to one request every ten seconds. Returns its teardown. */
  activate(): () => void {
    this.#messageTimer = setInterval(() => void this.#messageTick(), MESSAGE_MS)
    return () => {
      if (this.#messageTimer !== null) clearInterval(this.#messageTimer)
      this.#messageTimer = null
    }
  }

  // A forgotten tab in the background should not keep a laptop awake. Waking up polls
  // at once rather than waiting out the interval, so coming back shows current state.
  #onVisibility = () => {
    if (document.visibilityState !== 'visible') return
    void this.#roomTick()
    if (this.#messageTimer !== null) void this.#messageTick()
  }

  async #roomTick() {
    // A slow response must not stack requests behind it; skipping a tick is invisible
    // at this cadence, a queue of them is not.
    if (document.visibilityState === 'hidden' || this.#roomsInFlight) return
    this.#roomsInFlight = true
    try {
      await this.loadRooms()
    } finally {
      this.#roomsInFlight = false
    }
  }

  async #messageTick() {
    if (document.visibilityState === 'hidden' || this.#messagesInFlight) return
    if (this.openId === null) return
    this.#messagesInFlight = true
    try {
      await this.#pollMessages()
      // Membership changes are rare enough to ride along every fifth poll rather than
      // getting a request of their own.
      if (++this.#polls % MEMBERS_EVERY === 0) await this.loadMembers()
    } finally {
      this.#messagesInFlight = false
    }
  }

  async loadRooms() {
    try {
      this.rooms = await api.get<ChatRoom[]>('/api/chat/rooms')
      this.roomsError = ''
    } catch (e) {
      this.roomsError = e instanceof ApiError ? e.message : 'could not load rooms'
    } finally {
      this.loadingRooms = false
    }
  }

  async select(id: number | null) {
    if (id === this.openId) return
    this.openId = id
    this.messages = []
    this.members = null
    this.messagesError = ''
    this.#cursor = 0
    this.#marked = 0
    this.#changedSince = 0
    this.hasOlder = false
    if (id === null) return
    this.loadingMessages = true
    try {
      await Promise.all([this.#pollMessages(), this.loadMembers()])
    } finally {
      this.loadingMessages = false
    }
  }

  async loadMembers() {
    const room = this.openId
    if (room === null) return
    try {
      const members = await api.get<RoomMembers>(`/api/chat/rooms/${room}/members`)
      if (this.openId === room) this.members = members
    } catch {
      // The header falls back to the count it already has from the room list, and the
      // composer simply offers no completions. Neither is worth an error banner.
    }
  }

  async #pollMessages() {
    const room = this.openId
    if (room === null) return
    try {
      const changed = this.#changedSince ? `&changed=${this.#changedSince}` : ''
      const data = await api.get<{
        messages: ChatMessage[]
        changed?: ChatMessage[]
        has_older?: boolean
        last_read_id?: number
        now: number
      }>(`/api/chat/rooms/${room}/messages?since=${this.#cursor}${changed}`)
      // The room may have been switched while this was in flight; appending then would
      // drop someone else's conversation into the open one.
      if (this.openId !== room) return

      if (this.#cursor === 0) {
        this.messages = data.messages
        this.hasOlder = data.has_older ?? false
        this.lastReadAtOpen = data.last_read_id ?? 0
      } else {
        // Deduplicated rather than trusting the cursor: two polls can overlap around a
        // send, and a doubled message is worse than a wasted comparison.
        for (const incoming of data.messages) {
          if (!this.messages.some((m) => m.id === incoming.id)) this.messages.push(incoming)
        }
      }
      if (data.messages.length) {
        this.#cursor = Math.max(this.#cursor, ...data.messages.map((m) => m.id))
      }
      // Edits and removals of messages already held replace them where they stand.
      for (const updated of data.changed ?? []) {
        const at = this.messages.findIndex((m) => m.id === updated.id)
        if (at >= 0) this.messages[at] = updated
      }
      this.#changedSince = data.now
      this.messagesError = ''
      await this.#markRead(room)
    } catch (e) {
      this.messagesError = e instanceof ApiError ? e.message : 'could not load messages'
    }
  }

  /** Scrolling back: the page of history before the oldest message held. */
  async loadOlder() {
    const room = this.openId
    if (room === null || !this.hasOlder || this.loadingOlder || !this.messages.length) return
    this.loadingOlder = true
    try {
      const data = await api.get<{ messages: ChatMessage[]; has_older: boolean }>(
        `/api/chat/rooms/${room}/messages?before=${this.messages[0].id}`,
      )
      if (this.openId !== room) return
      this.messages = [...data.messages, ...this.messages]
      this.hasOlder = data.has_older
    } catch (e) {
      this.messagesError = e instanceof ApiError ? e.message : 'could not load earlier messages'
    } finally {
      this.loadingOlder = false
    }
  }

  async #markRead(room: number) {
    if (this.#cursor === 0 || this.#cursor === this.#marked) return
    const upTo = this.#cursor
    try {
      await api.post(`/api/chat/rooms/${room}/read`, { last_id: upTo })
      this.#marked = upTo
      // Clear the badge now rather than waiting for the next room poll, which is up to
      // ten seconds away and would leave an unread count on the room being read.
      const entry = this.rooms.find((r) => r.id === room)
      if (entry) entry.unread = 0
      void notify.refresh()
    } catch {
      // Cosmetic: the next successful poll will try again.
    }
  }

  /** Picks up what was just sent, without waiting out the tick. */
  async refresh() {
    if (this.openId !== null) await this.#pollMessages()
  }

  /** Picks up an edit or removal this client just made: it arrives as a change to a
   *  message already held, which the next poll would bring anyway — this is just sooner. */
  async reload() {
    await this.#pollMessages()
  }
}

export const chat = new ChatStore()
