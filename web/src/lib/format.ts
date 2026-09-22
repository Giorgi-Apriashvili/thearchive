export function bytes(value: number): string {
  if (value < 1024) return `${value} B`
  const units = ['KB', 'MB', 'GB', 'TB']
  let n = value / 1024
  let unit = 0
  while (n >= 1024 && unit < units.length - 1) {
    n /= 1024
    unit++
  }
  return `${n < 10 ? n.toFixed(1) : Math.round(n)} ${units[unit]}`
}

// Coarse on purpose: the only question a share link raises is roughly how long is left,
// and "in 29 days" reads better than a timestamp nobody will do arithmetic on.
export function until(epochSeconds: number): string {
  const seconds = epochSeconds - Date.now() / 1000
  if (seconds <= 0) return 'expired'
  const days = Math.floor(seconds / 86400)
  if (days >= 1) return `${days} day${days === 1 ? '' : 's'}`
  const hours = Math.floor(seconds / 3600)
  if (hours >= 1) return `${hours} hour${hours === 1 ? '' : 's'}`
  const minutes = Math.max(1, Math.floor(seconds / 60))
  return `${minutes} minute${minutes === 1 ? '' : 's'}`
}

// Chat timestamps. Time of day on its own is ambiguous the moment a conversation spans
// more than one day, so anything not from today carries its date as well.
export function chatTime(epochSeconds: number): string {
  const at = new Date(epochSeconds * 1000)
  const clock = at.toLocaleTimeString(undefined, { hour: '2-digit', minute: '2-digit' })
  if (at.toDateString() === new Date().toDateString()) return clock
  const day = at.toLocaleDateString(undefined, { day: 'numeric', month: 'short' })
  return `${day} ${clock}`
}

export function shortDate(epochSeconds: number): string {
  return new Date(epochSeconds * 1000).toLocaleDateString(undefined, {
    day: 'numeric',
    month: 'short',
    year: 'numeric',
  })
}
