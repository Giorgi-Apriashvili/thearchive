// A ~40 line router, rather than a dependency.
//
// The app previously had three routes and no client-side navigation, so matching the
// pathname once at startup was enough. The control panel breaks that: it has sections
// and per-user pages, and moving between them must not reload the page and lose the
// session state already in memory.
//
// Svelte 5's router ecosystem is still thin, and this needs exactly two things — a
// reactive current path, and pushState navigation that survives the back button.

class Router {
  path = $state(location.pathname)

  constructor() {
    // The back and forward buttons change the URL without going through go().
    addEventListener('popstate', () => (this.path = location.pathname))
  }

  go(to: string) {
    if (to === this.path) return
    history.pushState({}, '', to)
    this.path = to
    scrollTo(0, 0)
  }

  /** Segments of the current path, e.g. "/admin/users/3" -> ["admin","users","3"]. */
  get segments(): string[] {
    return this.path.split('/').filter(Boolean)
  }
}

export const router = new Router()

/** For <a> elements: navigate in-place, while leaving modified clicks to the browser
 *  so "open in new tab" still works. */
export function link(event: MouseEvent, to: string) {
  if (event.metaKey || event.ctrlKey || event.shiftKey || event.button !== 0) return
  event.preventDefault()
  router.go(to)
}
