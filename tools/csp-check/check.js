// Drives a real browser through every part of the app that does something a CSP
// governs, and records each securitypolicyviolation event per step. Ends with a
// deliberate violation as a control: a detector that never fires proves nothing.
const { chromium, request } = require('playwright')

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
  // Headless Chromium shows no notifications, so the page gets a stand-in that records
  // them, and a focus switch the test can flip to play "looking at another window".
  // Permission is kept in localStorage so it survives navigation, as a real grant would.
  await ctx.addInitScript(() => {
    window.__notes = []
    window.__focused = true
    document.hasFocus = () => window.__focused
    window.Notification = class {
      static get permission() {
        return localStorage.getItem('__perm') || 'default'
      }
      static async requestPermission() {
        localStorage.setItem('__perm', 'granted')
        return 'granted'
      }
      constructor(title, options = {}) {
        this.title = title
        this.body = options.body
        this.tag = options.tag
        this.onclick = null
        window.__notes.push(this)
      }
      close() {}
    }
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

  // Reached with no session, through the link on the sign-in page — which is how someone
  // without an account finds it. Also the first page here with a component <style> block.
  await step('privacy notice, from the sign-in page, signed out', async () => {
    await page.getByRole('link', { name: 'Privacy' }).click()
    await page.getByRole('heading', { name: 'How long it is kept' }).waitFor()
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

  await step('remove a finished upload, after confirming', async () => {
    const item = page.locator('li', { hasText: 'photo.jpg' }).first()
    await item.getByRole('button', { name: 'Remove', exact: true }).click()
    const dialog = page.getByRole('alertdialog')
    await dialog.getByRole('button', { name: 'Cancel' }).click()
    await dialog.waitFor({ state: 'detached' })
    await page.getByText(/Create link for 1 file/).waitFor()
    await item.getByRole('button', { name: 'Remove', exact: true }).click()
    await dialog.getByRole('button', { name: 'Remove file' }).click()
    await page.getByText(/Create link for \d+ file/).waitFor({ state: 'detached' })
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

  await step('change when a link expires', async () => {
    await page.getByText(/Expires .*, in \d+ days?\./).waitFor()
    await page.getByRole('combobox').filter({ hasText: 'from now' }).selectOption('365')
    await page.getByRole('button', { name: 'Save', exact: true }).click()
    await page.getByText('Saved', { exact: true }).waitFor()
    await page.getByText(/, in 365 days\./).waitFor()
  })

  await step('a link that never expires (admins and privileged members)', async () => {
    // Still expanded from the step before.
    const editor = page.locator('select').filter({ hasText: 'from now' })
    await editor.selectOption('0')
    await page.getByRole('button', { name: 'Save', exact: true }).click()
    await page.getByText('Never expires: its files stay').waitFor()
    await page.waitForFunction(() => document.body.innerText.includes('· never expires ·'))
    await editor.selectOption('365')
    await page.getByRole('button', { name: 'Save', exact: true }).click()
    await page.getByText(/, in 365 days\./).waitFor()
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
    // Within the popover: on a wide window the members panel says "creator" as well.
    await page.getByRole('group', { name: /^Members of / }).getByText('creator').waitFor()
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

  await step('account: edit profile and upload a picture', async () => {
    await page.getByLabel('Display name').fill('Alice Admin')
    await page.getByLabel('About you').fill('Keeper of the tent.\nAlso the snacks.')
    await page.getByRole('button', { name: 'Save profile' }).click()
    await page.getByText('Saved.').waitFor()
    await page.setInputFiles('input[type=file][accept="image/*"]', PHOTO)
    // The picture is shown once the session reloads with it.
    await page.waitForFunction(() =>
      [...document.images].some((i) => i.src.includes('/api/avatars/') && i.complete && i.naturalWidth > 0),
    )
  })

  await step('own profile (picture, bio, rooms)', async () => {
    await page.goto('/u/alice')
    await page.getByText('Keeper of the tent.').waitFor()
    await page.getByRole('heading', { name: 'Your rooms' }).waitFor()
  })

  // bob has no picture: the initials circle's colour is set with `style:`, which must go
  // through the CSSOM and not be refused as an inline style.
  await step("another member's profile (initials, style: binding)", async () => {
    await page.goto('/u/bob')
    await page.getByRole('heading', { name: 'Rooms you share' }).waitFor()
  })

  await step('chat: messages with a picture and a display name', async () => {
    await page.goto('/chat')
    await page.getByText('CSP test').first().click()
    await page.getByText('Alice Admin').first().waitFor()
    await page.waitForFunction(() =>
      [...document.images].some((i) => i.src.includes('/api/avatars/') && i.complete && i.naturalWidth > 0),
    )
  })

  await step('chat: attach a link, and its card with a preview', async () => {
    await page.getByRole('button', { name: 'Attach your links' }).click()
    await page.getByRole('dialog').getByText('Night out').click()
    await page.getByRole('dialog').getByRole('button', { name: 'Attach', exact: true }).click()
    await page.getByRole('button', { name: 'Send' }).click()
    await page.waitForFunction(() =>
      [...document.querySelectorAll('a[href^="/d/"] img')].some((i) => i.complete && i.naturalWidth > 0),
    )
  })

  await step('profile: send two links to bob, with a note', async () => {
    await page.goto('/u/bob')
    await page.getByRole('button', { name: 'Send links' }).click()
    await page.getByRole('dialog').getByText('Night out').click()
    await page.getByRole('dialog').getByText('Day trip').click()
    await page.getByRole('dialog').locator('textarea').fill('from the weekend')
    await page.getByRole('button', { name: 'Send', exact: true }).click()
    await page.getByText(/^Sent 2 links/).waitFor()
  })

  await step('inbox: bob sees what was sent to him', async () => {
    await page.request.post('/api/auth/login', { data: { username: 'bob', password: PASS } })
    try {
      await page.goto('/')
      await page.getByRole('heading', { name: 'Shared with you' }).waitFor()
      // One item: the note once, both links under it.
      const item = page.locator('li', { hasText: 'from the weekend' })
      await item.waitFor()
      await item.getByText('Night out').waitFor()
      await item.getByText('Day trip').waitFor()
    } finally {
      await page.request.post('/api/auth/login', { data: { username: 'alice', password: PASS } })
    }
  })

  await step('uploads: files left without a link are offered again', async () => {
    // Uploaded straight through tus, as a tab closed before "Create link" leaves them.
    const body = require('fs').readFileSync(PHOTO)
    const created = await page.request.post('/files', {
      headers: {
        'Tus-Resumable': '1.0.0',
        'Upload-Length': String(body.length),
        'Upload-Metadata': `filename ${Buffer.from('left-behind.jpg').toString('base64')}`,
      },
    })
    await page.request.patch(created.headers()['location'], {
      headers: {
        'Tus-Resumable': '1.0.0',
        'Upload-Offset': '0',
        'Content-Type': 'application/offset+octet-stream',
      },
      data: body,
    })
    await page.goto('/')
    await page.getByText('left-behind.jpg').waitFor()
    await page.getByText(/never made a link/).waitFor()
    await page.getByRole('button', { name: /Create link for \d+ file/ }).click()
    await page.getByText(/Link created/).waitFor()
  })

  await step('notifications: offered once, then on', async () => {
    await page.goto('/')
    await page.getByText(/Get a notification when someone mentions you/).waitFor()
    await page.getByRole('button', { name: 'Turn on' }).click()
    await page.getByText(/Get a notification when someone mentions you/).waitFor({ state: 'detached' })
    await page.goto('/account')
    const section = page.locator('section', { hasText: 'Notifications on this device' })
    await section.getByText('On', { exact: true }).waitFor()
  })

  await step('notifications: an invitation and a mention pop up while away', async () => {
    await page.goto('/')
    await page.waitForTimeout(1000) // the first poll, which only learns what is already there
    const bob = await request.newContext({ baseURL: BASE })
    try {
      await bob.post('/api/auth/login', { data: { username: 'bob', password: PASS } })
      const room = await (await bob.post('/api/chat/rooms', { data: { name: 'Pop-up test' } })).json()
      await bob.post(`/api/chat/rooms/${room.id}/invite`, { data: { username: 'alice' } })
      // Looking at something else; a visibility event makes the poll run now, not in 30s.
      const away = () =>
        page.evaluate(() => {
          window.__focused = false
          document.dispatchEvent(new Event('visibilitychange'))
        })
      const popped = (text) =>
        page.waitForFunction((t) => window.__notes.some((n) => n.title.includes(t)), text)
      await away()
      await popped('bob invited you to Pop-up test')
      await page.waitForFunction(() => /^\(\d+\) WeekendArchive$/.test(document.title))

      await page.request.post(`/api/chat/rooms/${room.id}/respond`, { data: { action: 'accept' } })
      await bob.post(`/api/chat/rooms/${room.id}/messages`, { data: { body: '@alice the tickets are booked' } })
      await away()
      await popped('bob mentioned you in Pop-up test')
      const body = await page.evaluate(() => window.__notes.at(-1).body)
      if (body !== '@alice the tickets are booked') throw new Error(`unexpected body: ${body}`)
      // Clicking it opens the room.
      await page.evaluate(() => window.__notes.at(-1).onclick())
      await page.waitForURL(`**/chat/${room.id}`)
      await page.getByPlaceholder('Message Pop-up test').waitFor()

      // While looking at the site, the badges say it; nothing pops up.
      await page.evaluate(() => (window.__focused = true))
      await bob.post(`/api/chat/rooms/${room.id}/messages`, { data: { body: '@alice while you look' } })
      await page.evaluate(() => document.dispatchEvent(new Event('visibilitychange')))
      await page.waitForTimeout(1500)
      const shown = await page.evaluate(() => window.__notes.filter((n) => n.body?.includes('while you look')).length)
      if (shown !== 0) throw new Error('a notification popped up while the site was in focus')
    } finally {
      await page.evaluate(() => (window.__focused = true))
      await bob.dispose()
    }
  })

  await step('control panel', async () => {
    await page.goto('/admin')
    await page.getByText('Users').first().waitFor()
  })

  await step('control panel: activity', async () => {
    await page.goto('/admin/activity')
    await page.getByText('Signed in', { exact: true }).first().waitFor()
    await page.locator('select').selectOption('link')
    await page.getByText('Made a link', { exact: true }).first().waitFor()
    const shown = await page.locator('main li span:first-child').allInnerTexts()
    if (shown.some((t) => t === 'Signed in')) throw new Error('the Links filter still shows sign-ins')
  })

  await step('storage limits: your space, an admin sets one, an upload over it is refused', async () => {
    await page.goto('/')
    await page.getByText('Your space').waitFor()
    await page.getByText(/· no limit/).waitFor() // alice is an administrator
    const users = await (await page.request.get('/api/admin/users')).json()
    const bob = users.find((u) => u.username === 'bob')
    await page.goto(`/admin/users/${bob.id}`)
    const section = page.locator('section', { hasText: 'Storage' }).filter({ has: page.getByPlaceholder('GB') })
    await section.getByPlaceholder('GB').fill('0.0001')
    await section.getByRole('button', { name: 'Set limit' }).click()
    await section.getByText(/set for this account/).waitFor()
    await page.request.post('/api/auth/login', { data: { username: 'bob', password: PASS } })
    try {
      await page.goto('/')
      await page.setInputFiles('input[type=file]', PHOTO)
      await page.getByText(/this would take you over your \d+ KB storage limit/).waitFor()
      if (await page.getByRole('button', { name: 'Retry', exact: true }).count())
        throw new Error('offered Retry for an upload refused for space')
    } finally {
      await page.request.post('/api/auth/login', { data: { username: 'alice', password: PASS } })
      await page.request.post(`/api/admin/users/${bob.id}/limit`, { data: { bytes: null } })
    }
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

  await step('privacy notice, from a download page footer', async () => {
    await page.goto(`/d/${TOKEN}`)
    await page.getByRole('link', { name: 'Privacy' }).click()
    await page.getByRole('heading', { name: 'Who can see it' }).waitFor()
  })

  await step('remove one file from a link, after confirming', async () => {
    await page.goto('/')
    await page.getByRole('button', { name: /Day trip/ }).click()
    const files = page.locator('li', { hasText: 'Day trip' }).locator('ul li')
    await files.first().waitFor()
    await files.first().getByRole('button', { name: 'Remove', exact: true }).click()
    const dialog = page.getByRole('alertdialog')
    // Day trip holds one file, so this is the variant that also ends the link — cancel it.
    await dialog.getByRole('button', { name: 'Remove file and link' }).waitFor()
    await dialog.getByRole('button', { name: 'Cancel' }).click()
    await dialog.waitFor({ state: 'detached' })
    await files.first().waitFor()
  })

  await step('remove a whole link, after confirming', async () => {
    await page.goto('/')
    const row = page.locator('li', { hasText: 'Day trip' }).filter({ has: page.getByRole('button', { name: 'Remove', exact: true }) }).first()
    await row.getByRole('button', { name: 'Remove', exact: true }).first().click()
    const dialog = page.getByRole('alertdialog')
    await dialog.getByText(/cannot be undone/).waitFor()
    await dialog.getByRole('button', { name: 'Remove link' }).click()
    await page.getByRole('button', { name: /Day trip/ }).waitFor({ state: 'detached' })
  })

  await step('add files to an existing link', async () => {
    await page.goto('/')
    const row = page.locator('li', { hasText: 'Night out' }).filter({ has: page.getByRole('link', { name: 'Open', exact: true }) }).first()
    const before = Number((await row.innerText()).match(/(\d+) files?/)[1])
    await page.getByRole('button', { name: /Night out/ }).first().click()
    const [chooser] = await Promise.all([
      page.waitForEvent('filechooser'),
      page.getByRole('button', { name: 'Add files', exact: true }).click(),
    ])
    await chooser.setFiles(PHOTO)
    await page.getByRole('button', { name: /^Add 1 file to “Night out”/ }).click()
    await page.getByText(/^Added 1 file to “Night out”/).waitFor()
    await row.getByText(new RegExp(`^${before + 1} files`)).waitFor()
    // The destination resets: the next upload is a new link again unless chosen otherwise.
    await page.setInputFiles('input[type=file]', PHOTO)
    await page.getByRole('button', { name: /^Create link for 1 file/ }).waitFor({ timeout: 15000 })
    const item = page.locator('#upload-list li').first()
    await item.getByRole('button', { name: 'Remove', exact: true }).click()
    await page.getByRole('alertdialog').getByRole('button', { name: 'Remove file' }).click()
  })

  await step('who downloaded a link', async () => {
    const detail = await (await page.request.get(`/api/shares/${TOKEN}`)).json()
    const file = detail.files[0].id
    const bob = await request.newContext({ baseURL: BASE })
    const stranger = await request.newContext({ baseURL: BASE })
    try {
      await bob.post('/api/auth/login', { data: { username: 'bob', password: PASS } })
      await bob.get(`/d/${TOKEN}/${file}`)
      await stranger.get(`/d/${TOKEN}/${file}`)
    } finally {
      await bob.dispose()
      await stranger.dispose()
    }
    await page.goto('/')
    await page.getByRole('button', { name: /Night out/ }).first().click()
    const panel = page.locator('li', { hasText: 'Downloaded by' }).first()
    await panel.getByText('bob', { exact: true }).waitFor()
    const text = (await panel.innerText()).replace(/\s+/g, ' ')
    for (const want of [/bob · 1 download · /, /1 download by people who were not signed in · last /]) {
      if (!want.test(text)) throw new Error(`expected ${want} in: ${text}`)
    }
  })

  // Starts an upload of PHOTO straight against the server, as a tab that was closed
  // part-way would have left it, and sends the first half. Returns its id.
  async function halfUpload() {
    const fs = require('fs')
    const body = fs.readFileSync(PHOTO)
    const meta = [
      `filename ${Buffer.from('photo.jpg').toString('base64')}`,
      `lastModified ${Buffer.from(String(Math.floor(fs.statSync(PHOTO).mtimeMs))).toString('base64')}`,
    ].join(',')
    const created = await page.request.post('/files', {
      headers: { 'Tus-Resumable': '1.0.0', 'Upload-Length': String(body.length), 'Upload-Metadata': meta },
    })
    const url = created.headers()['location']
    await page.request.patch(url, {
      headers: { 'Tus-Resumable': '1.0.0', 'Upload-Offset': '0', 'Content-Type': 'application/offset+octet-stream' },
      data: body.subarray(0, Math.floor(body.length / 2)),
    })
    return { id: url.split('/').pop(), half: Math.floor(body.length / 2) }
  }

  await step('an interrupted upload continues where it stopped', async () => {
    const { id, half } = await halfUpload()
    await page.goto('/')
    await page.getByText(/stopped part-way/).waitFor()
    // Half, rounded down: 49% when the generated image has an odd size.
    await page.getByText(/· (49|50)% of/).waitFor()
    const offsets = []
    page.on('request', (r) => {
      if (r.method() === 'PATCH' && r.url().endsWith(id)) offsets.push(Number(r.headers()['upload-offset']))
    })
    await page.setInputFiles('input[type=file]', PHOTO)
    await page.getByText(/stopped part-way/).waitFor({ state: 'detached' })
    await page.getByRole('button', { name: /^Create link for 1 file/ }).waitFor({ timeout: 15000 })
    if (offsets[0] !== half) throw new Error(`expected to continue from ${half}, sent from ${offsets[0]}`)
    const pending = await (await page.request.get('/api/uploads')).json()
    if (!pending.uploads.some((u) => u.id === id)) throw new Error('the upload finished under a new id instead')
    const item = page.locator('#upload-list li').first()
    await item.getByRole('button', { name: 'Remove', exact: true }).click()
    await page.getByRole('alertdialog').getByRole('button', { name: 'Remove file' }).click()
  })

  await step('a failed upload can be retried', async () => {
    await page.goto('/')
    await page.route('**/files/*', (route) =>
      route.request().method() === 'PATCH' ? route.abort('internetdisconnected') : route.continue(),
    )
    await page.setInputFiles('input[type=file]', PHOTO)
    // tus retries on its own first, for about twenty seconds, then gives up.
    const retry = page.getByRole('button', { name: 'Retry', exact: true })
    await retry.waitFor({ timeout: 40000 })
    await page.unroute('**/files/*')
    await retry.click()
    await page.getByRole('button', { name: /^Create link for 1 file/ }).waitFor({ timeout: 15000 })
    const item = page.locator('#upload-list li').first()
    await item.getByRole('button', { name: 'Remove', exact: true }).click()
    await page.getByRole('alertdialog').getByRole('button', { name: 'Remove file' }).click()
  })

  await step('an unfinished upload can be discarded', async () => {
    const { id } = await halfUpload()
    await page.goto('/')
    await page.getByRole('button', { name: 'Discard', exact: true }).click()
    await page.getByRole('alertdialog').getByRole('button', { name: 'Discard' }).click()
    await page.getByText(/stopped part-way/).waitFor({ state: 'detached' })
    const pending = await (await page.request.get('/api/uploads')).json()
    if (pending.unfinished.some((u) => u.id === id)) throw new Error('still on the server')
  })

  await step('leaving mid-upload asks first; afterwards it does not', async () => {
    await page.goto('/')
    // Hold the upload mid-transfer: its PATCH requests wait until released.
    const held = []
    await page.route('**/files/*', (route) =>
      route.request().method() === 'PATCH' ? held.push(route) : route.continue(),
    )
    await page.locator('body').click() // browsers only prompt on pages the user has touched
    await page.setInputFiles('input[type=file]', PHOTO)
    await page.getByText(/^\d+%$/).first().waitFor()

    let prompted = null
    page.once('dialog', (d) => {
      prompted = d.type()
      void d.dismiss()
    })
    await page.reload({ timeout: 3000 }).catch(() => {})
    if (prompted !== 'beforeunload') throw new Error(`reload mid-upload: expected a beforeunload prompt, got ${prompted}`)
    await page.getByText(/^\d+%$/).first().waitFor() // stayed, upload still listed

    await page.getByRole('button', { name: 'Sign out' }).click()
    const dialog = page.getByRole('alertdialog')
    await dialog.getByText('Sign out while uploading?').waitFor()
    await dialog.getByRole('button', { name: 'Cancel' }).click()

    // Let it finish; tus retries the held request.
    await page.unroute('**/files/*')
    for (const route of held) await route.abort().catch(() => {})
    await page.getByText(/Create link for \d+ file/).waitFor({ timeout: 20000 })
    let promptedAfter = false
    const onDialog = (d) => {
      promptedAfter = true
      void d.dismiss()
    }
    page.on('dialog', onDialog)
    await page.reload()
    page.off('dialog', onDialog)
    if (promptedAfter) throw new Error('prompted on reload after the upload had finished')
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
