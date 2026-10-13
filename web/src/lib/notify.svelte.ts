// Pop-up notifications while the site is open — in any tab, foreground or not — for
// mentions of you, links sent to you and room invitations; and the count of those in
// the tab title.
//
// One poll of GET /api/notifications every thirty seconds, kept running while the tab is
// hidden: unlike chat's polls, which pause then, this one exists for exactly that case.
// Whatever is already waiting when the page loads is taken as known — opening the site
// is not news — so only what arrives afterwards pops up, and only while you are not
// looking at the site, where the badges already say it.
//
// The browser permission is per site; whether to use it is per device, kept here so it
// can be switched off without the browser's settings. Nothing leaves the browser: the
// notifications are the browser's own, with no push service involved, which is also why
// nothing arrives once every tab is closed.

import { api, type NotificationItem, type Notifications } from './api'
import { router } from './router.svelte'

const POLL_MS = 30_000
const BASE_TITLE = 'WeekendArchive'
const PREF_KEY = 'thearchive.notifications'
const ASKED_KEY = 'thearchive.notifications-asked'
const SHOWN_KEY = 'thearchive.notifications-shown'

function read(key: string): string | null {
  try {
    return localStorage.getItem(key)
  } catch {
    return null
  }
}

function write(key: string, value: string) {
  try {
    localStorage.setItem(key, value)
  } catch {
    // Private mode or storage blocked: the preference lasts for this page only.
  }
}

class NotifyStore {
  readonly supported = typeof window !== 'undefined' && 'Notification' in window
  permission = $state<NotificationPermission>(this.supported ? Notification.permission : 'denied')
  #pref = $state(read(PREF_KEY))
  #asked = $state(read(ASKED_KEY) === '1')
  total = $state(0)

  #known = new Set<string>()
  #seeded = false
  #timer: ReturnType<typeof setInterval> | null = null
  #inFlight = false

  /** On for this device: the browser allows it and it has not been switched off here. */
  get enabled() {
    return this.permission === 'granted' && this.#pref !== 'off'
  }

  /** Worth offering: supported, never answered, and not waved away before. */
  get shouldOffer() {
    return this.supported && this.permission === 'default' && !this.#asked
  }

  /** Must run from a click: browsers refuse a permission prompt that is not one. */
  async enable() {
    if (!this.supported) return
    this.permission = await Notification.requestPermission()
    this.#pref = 'on'
    write(PREF_KEY, 'on')
    this.dismissOffer()
  }

  disable() {
    this.#pref = 'off'
    write(PREF_KEY, 'off')
  }

  dismissOffer() {
    this.#asked = true
    write(ASKED_KEY, '1')
  }

  watch(): () => void {
    void this.refresh()
    this.#timer = setInterval(() => void this.refresh(), POLL_MS)
    document.addEventListener('visibilitychange', this.#onVisibility)
    return () => {
      if (this.#timer !== null) clearInterval(this.#timer)
      this.#timer = null
      document.removeEventListener('visibilitychange', this.#onVisibility)
      this.#known.clear()
      this.#seeded = false
      this.total = 0
      document.title = BASE_TITLE
    }
  }

  #onVisibility = () => {
    // Permission can be changed in the browser's own settings while the tab was away.
    if (this.supported) this.permission = Notification.permission
    if (document.visibilityState === 'visible') void this.refresh()
  }

  /** Polls now. Also called after reading a room or opening "Shared with you", so the
   *  title count drops at once rather than at the next tick. */
  async refresh() {
    if (this.#inFlight) return
    this.#inFlight = true
    try {
      const data = await api.get<Notifications>('/api/notifications')
      const { mentions, links, invitations } = data.counts
      this.total = mentions + links + invitations
      document.title = this.total > 0 ? `(${this.total}) ${BASE_TITLE}` : BASE_TITLE

      const fresh = data.items.filter((item) => !this.#known.has(item.key))
      for (const item of data.items) this.#known.add(item.key)
      if (!this.#seeded) {
        this.#seeded = true
        return
      }
      // Oldest first, so they stack in the order they happened.
      for (const item of fresh.reverse()) this.#show(item)
    } catch {
      // A missed poll changes nothing; the next one tries again.
    } finally {
      this.#inFlight = false
    }
  }

  #show(item: NotificationItem) {
    if (!this.enabled) return
    // Looking at the site already: the badges say it, and a pop-up would only repeat it.
    if (document.visibilityState === 'visible' && document.hasFocus()) return
    // Several open tabs each poll; the first to see an item claims it, so it pops up once.
    if (!this.#claim(item.key)) return

    const { title, body, url } = describe(item)
    try {
      const shown = new Notification(title, { body, tag: item.key, icon: '/apple-touch-icon.png' })
      shown.onclick = () => {
        window.focus()
        router.go(url)
        shown.close()
      }
    } catch {
      // Some mobile browsers expose Notification but only allow it from a service worker.
    }
  }

  #claim(key: string): boolean {
    let shown: string[] = []
    try {
      shown = JSON.parse(read(SHOWN_KEY) ?? '[]')
    } catch {
      shown = []
    }
    if (shown.includes(key)) return false
    write(SHOWN_KEY, JSON.stringify([...shown, key].slice(-200)))
    return true
  }
}

function nameOf(from: NotificationItem['from']): string {
  return from ? from.display_name || from.username : 'Someone'
}

function describe(item: NotificationItem): { title: string; body: string; url: string } {
  const who = nameOf(item.from)
  switch (item.kind) {
    case 'mention':
      return {
        title: `${who} mentioned you in ${item.room?.name ?? 'a room'}`,
        body: item.excerpt ?? '',
        url: `/chat/${item.room?.id ?? ''}`,
      }
    case 'link': {
      const titles = (item.titles ?? []).map((t) => t || 'Untitled')
      const count = titles.length
      return {
        title: `${who} sent you ${count === 1 ? 'a link' : `${count} links`}`,
        body: [item.note, titles.join(', ')].filter(Boolean).join('\n'),
        url: '/',
      }
    }
    case 'invitation':
      return {
        title: item.from
          ? `${who} invited you to ${item.room?.name ?? 'a room'}`
          : `You were invited to ${item.room?.name ?? 'a room'}`,
        body: 'Open Chat to accept or decline.',
        url: '/chat',
      }
  }
}

export const notify = new NotifyStore()
