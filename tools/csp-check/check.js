// Drives a real browser through every part of the app that does something a CSP
// governs, and records each securitypolicyviolation event per step. Ends with a
// deliberate violation as a control: a detector that never fires proves nothing.
const { chromium } = require('playwright')

const { BASE, TOKEN, PHOTO } = process.env
const PASS = 'correct-horse-battery'

;(async () => {
  const browser = await chromium.launch()
  const ctx = await browser.newContext({ baseURL: BASE })
  // Installed before any page script runs, so violations during load are caught too.
  await ctx.addInitScript(() => {
    window.__csp = []
    document.addEventListener('securitypolicyviolation', (e) =>
      window.__csp.push(`${e.effectiveDirective} blocked ${e.blockedURI || '(inline)'}`),
    )
  })
  const page = await ctx.newPage()
  const errors = []
  page.on('pageerror', (e) => errors.push(`pageerror: ${e.message}`))
  page.on('console', (m) => m.type() === 'error' && errors.push(`console: ${m.text()}`))

  let unexpected = 0
  async function step(name, fn, { expectViolation = false } = {}) {
    try {
      await fn()
    } catch (e) {
      console.log(`  ERROR     ${name}: ${e.message.split('\n')[0]}`)
      unexpected++
      return
    }
    await page.waitForTimeout(500) // let transitions and polls run
    const found = await page.evaluate(() => {
      const seen = window.__csp
      window.__csp = []
      return seen
    })
    const bad = expectViolation ? found.length === 0 : found.length > 0
    if (bad) unexpected++
    const label = found.length ? 'VIOLATION' : 'clean    '
    console.log(`  ${label} ${name}${found.length ? '  -> ' + found.join(' | ') : ''}`)
  }

  // Script blocked by the policy would leave #app empty; every step waits for real UI.
  await step('sign-in page', async () => {
    await page.goto('/')
    await page.getByText('Sign in', { exact: false }).first().waitFor()
  })

  await page.request.post('/api/auth/login', { data: { username: 'alice', password: PASS } })

  await step('uploads page, storage bar (style= binding)', async () => {
    await page.goto('/')
    await page.getByText('Storage').waitFor()
  })

  await step('upload with progress bar (style= binding)', async () => {
    await page.setInputFiles('input[type=file]', PHOTO)
    await page.getByText(/Create link for 1 file/).waitFor({ timeout: 15000 })
  })

  await step('confirm dialog (fade + scale transitions)', async () => {
    await page.getByRole('button', { name: 'Invite' }).click()
    await page.getByRole('alertdialog').waitFor()
    await page.getByRole('button', { name: 'Cancel' }).last().click()
  })

  await step('expand a share in "Your links"', async () => {
    await page.getByText('Night').first().click()
    await page.getByText('photo.jpg').first().waitFor()
  })

  await step('chat: create a room', async () => {
    await page.goto('/chat')
    await page.getByPlaceholder('New room').fill('CSP test')
    await page.getByRole('button', { name: 'Create' }).click()
    await page.getByPlaceholder('Message CSP test').waitFor()
  })

  await step('chat: message with a link and an @mention', async () => {
    await page.getByPlaceholder('Message CSP test').fill('see https://example.com @bob')
    await page.keyboard.press('Escape') // dismiss any autocomplete
    await page.getByRole('button', { name: 'Send' }).click()
    await page.getByRole('link', { name: 'https://example.com' }).waitFor()
  })

  await step('chat: @ autocomplete popup', async () => {
    await page.getByPlaceholder('Message CSP test').fill('@b')
    await page.waitForTimeout(300)
    await page.getByPlaceholder('Message CSP test').fill('')
  })

  await step('chat: member popover (fade transition)', async () => {
    // The header's count is the toggle; the room list shows the same text as plain info.
    await page.locator('button[aria-expanded]').click()
    await page.getByText('creator').waitFor()
    await page.keyboard.press('Escape')
  })

  await step('chat: decline dialog (fade + scale transitions)', async () => {
    // alice is admin: invite bob to a second room, then act as bob for the decline.
    const room = await (await page.request.post('/api/chat/rooms', { data: { name: 'Second' } })).json()
    await page.request.post(`/api/chat/rooms/${room.id}/invite`, { data: { username: 'bob' } })
    await page.request.post('/api/auth/login', { data: { username: 'bob', password: PASS } })
    try {
      await page.goto('/chat')
      await page.getByRole('button', { name: 'Decline' }).click()
      // Scoped to the dialog: the backdrop behind it is also a button labelled Cancel.
      await page.getByRole('dialog').getByRole('button', { name: 'Cancel' }).click()
    } finally {
      await page.request.post('/api/auth/login', { data: { username: 'alice', password: PASS } })
    }
  })

  await step('account page', async () => {
    await page.goto('/account')
    await page.getByText('Change your password').waitFor()
  })

  await step('control panel', async () => {
    await page.goto('/admin')
    await page.getByText('Users').first().waitFor()
  })

  await step('control panel: a user', async () => {
    await page.goto('/admin/users/1')
    await page.getByText('Reset password').first().waitFor()
  })

  await step('download page with thumbnails', async () => {
    await page.goto(`/d/${TOKEN}`)
    await page.getByRole('button', { name: /Preview/ }).first().waitFor()
  })

  await step('lightbox (fade + scale transitions, /thumb image)', async () => {
    await page.getByRole('button', { name: /Preview/ }).first().click()
    await page.waitForFunction(() =>
      [...document.images].some((i) => i.src.includes('/thumb') && i.complete && i.naturalWidth > 0),
    )
    await page.keyboard.press('Escape')
  })

  // The control. An inline style attribute set through the DOM is exactly what the
  // policy forbids; if this is not reported, nothing above means anything.
  await step(
    'CONTROL: deliberate inline style attribute',
    async () => {
      await page.evaluate(() => document.body.setAttribute('style', 'outline: 1px solid red'))
    },
    { expectViolation: true },
  )

  // Give the browser a moment to POST the control's report to the server.
  await page.waitForTimeout(1500)
  await browser.close()

  const relevant = errors.filter((e) => !e.includes('401') && !e.includes('Failed to load resource') && !e.includes('Content Security Policy'))
  console.log(`\n  page errors: ${relevant.length ? '\n    ' + relevant.join('\n    ') : 'none'}`)
  console.log(`\n  ${unexpected === 0 ? 'ALL AS EXPECTED' : unexpected + ' UNEXPECTED'}`)
  process.exit(unexpected === 0 ? 0 : 1)
})()
