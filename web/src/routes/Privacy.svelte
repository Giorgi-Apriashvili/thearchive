<script lang="ts">
  import { api, type PrivacyInfo } from '../lib/api'

  // Every statement below was checked against the code, and every period comes from
  // /api/privacy, which reports the values the server enforces. Change what this page
  // says only together with what the system does — and move this date when you do.
  const UPDATED = '23 September 2026'

  let info = $state<PrivacyInfo | null>(null)
  let failed = $state(false)

  api
    .get<PrivacyInfo>('/api/privacy')
    .then((loaded) => (info = loaded))
    .catch(() => (failed = true))

  function days(n: number): string {
    return `${n} day${n === 1 ? '' : 's'}`
  }
  function hours(n: number): string {
    return `${n} hour${n === 1 ? '' : 's'}`
  }
  function minutes(n: number): string {
    return `${n} minute${n === 1 ? '' : 's'}`
  }
</script>

<div class="mx-auto max-w-2xl px-4 py-10">
  <!-- A full navigation, not a router link: someone may have landed here from a link
       with no session loaded, and "/" should come up as whatever it is for them. -->
  <a href="/" class="wordmark text-2xl transition hover:opacity-80">Weekend<em>Archive</em></a>

  <h1 class="mt-8 text-xl font-semibold tracking-tight">Privacy</h1>
  <p class="mt-1 text-sm text-ink-500">
    What this site keeps about you, who can see it, and for how long. Last updated {UPDATED}.
  </p>

  {#if failed}
    <p class="mt-8 rounded-lg bg-red-950/60 px-3 py-2 text-sm text-red-300">
      The details of this site's settings could not be loaded. Try again in a moment.
    </p>
  {:else if !info}
    <p class="mt-8 text-sm text-ink-500">Loading…</p>
  {:else}
    {@const r = info.retention}
    <div class="notice mt-8 space-y-8 text-sm leading-relaxed text-ink-300">
      <section>
        <h2>Who runs this</h2>
        {#if info.operator}
          <p>
            WeekendArchive is run by {info.operator.name || 'its operator'}.
            {#if info.operator.contact}
              For anything about your data — a copy of it, a correction, or deleting it —
              email <a href="mailto:{info.operator.contact}">{info.operator.contact}</a>.
            {/if}
          </p>
        {:else}
          <p>
            Whoever runs this copy of WeekendArchive has not published their details here.
            Ask the person who invited you.
          </p>
        {/if}
        <p>
          It runs on a single rented server{#if info.hosting_location}&nbsp;in {info.hosting_location}{/if}.
          Nothing described below is copied to any other service. As with anything hosted,
          the company that provides the server could in principle reach it.
        </p>
      </section>

      <section>
        <h2>What it is</h2>
        <p>
          A private place for a small group of friends to share files by link and talk about
          them. Membership is by invitation only. It is not advertised, carries no ads, and
          asks search engines and AI crawlers not to visit.
        </p>
      </section>

      <section>
        <h2>What is stored</h2>
        <ul>
          <li>
            <strong>Your account:</strong> your username; your password, but only as a one-way
            Argon2id hash that nobody can read back, the operator included; your role; when you
            joined; and who invited you.
          </li>
          <li>
            <strong>Your profile</strong>, if you fill it in: a display name, a few lines about
            yourself, and a picture. Only a small square copy of the picture is kept — the file
            you uploaded is discarded, and with it everything inside it, such as where a photo
            was taken.
          </li>
          <li>
            <strong>Signing in:</strong> one cookie, holding a random session identifier. It is
            needed for the site to work, so there is no cookie banner, and it is the only cookie
            the site sets. A session lasts {days(r.session_days)} or until you sign out.
          </li>
          <li>
            <strong>What you upload:</strong> the files, and with each one its name, type and
            size, its last-modified time and folder path as your browser reports them, and who
            uploaded it. Until an upload is made into a link, the identification string your
            browser sent with it (its User-Agent) is kept too. Previews are made of images.
          </li>
          <li>
            <strong>Links you create:</strong> their title, expiry, whether they are public or
            members-only, any download limit, how many times they have been downloaded, and a
            password if you set one — again only as a hash.
          </li>
          <li>
            <strong>Links you send:</strong> which of your links you sent to whom, when, with
            any note, and whether they have looked at it yet; and which links you attached to
            chat messages.
          </li>
          <li>
            <strong>Chat:</strong> your messages, the rooms, invitations and how you answered
            them, anyone you have blocked, who a message @mentioned, and how far you have read.
          </li>
          <li>
            <strong>Records kept for security and upkeep:</strong> failed password attempts, with
            the network address they came from and the username or link they were for;
            administrators' actions on accounts; a link's address, its creator and its size
            when it is created, and its address and size when it is downloaded as a ZIP; the
            size and a fingerprint of the contents of each finished upload, but not its name;
            and, if a page ever breaks the
            site's content security policy, that page's address. <em>Ordinary visits are not
            logged</em> — there are no access logs.
          </li>
        </ul>
      </section>

      <section>
        <h2>Who can see it</h2>
        <ul>
          <li>
            <strong>Members-only links</strong>, the default: anyone signed in to this site.
          </li>
          <li>
            <strong>Public links:</strong> anyone who has the link, account or not. Treat a public
            link like the files themselves.
          </li>
          <li>
            <strong>Profiles</strong> — your display name, about text, picture, when you joined
            and who invited you — are visible to every signed-in member, never to someone who
            only has a link. A profile lists the chat rooms its viewer shares with you, not all
            of yours. Your username is always shown beside your display name, so nobody can pass
            as you by choosing the same one.
          </li>
          <li>
            <strong>Links you send</strong> are seen by the person you send them to, and links
            you attach in chat by that room's members — their title, size, and small previews of
            their images. Sending a link changes nothing about who can open it: a members-only
            link opens for any member, and one with a password still needs it. Someone who has
            blocked you in chat does not receive links from you either.
          </li>
          <li>
            <strong>Chat:</strong> every member can see the names of all rooms; what is said in a
            room only its members can see.
          </li>
          <li>
            <strong>Administrators</strong> can see the list of accounts and every link and file,
            and can remove chat messages. They can add themselves to any room, which then shows
            in its member count like anyone else's joining.
          </li>
          <li>
            <strong>The operator</strong> has access to the server itself, and so to everything
            on it, including members-only files. Passwords remain the exception: they cannot
            be read back from how they are stored.
          </li>
        </ul>
      </section>

      <section>
        <h2>How long it is kept</h2>
        <ul>
          <li>
            <strong>Files</strong> stay until the link they are in expires — after
            {days(r.link_default_days)} unless its creator chose otherwise, and never more than
            {days(r.link_max_days)} — or is revoked, whichever comes first. They are then deleted
            within {minutes(r.sweep_minutes)}, unless the same file is still in another link that
            has not expired. Previews go with their file. Anything uploaded but never made into a
            link is deleted within {hours(r.unshared_upload_hours)}.
          </li>
          <li>
            <strong>A link's record</strong> — its title, dates and download count, but not its
            files — is kept after it expires, as history.
          </li>
          <li>
            <strong>Chat is kept permanently.</strong> If an account is deleted, its messages stay
            in their rooms under the name the person last had, so conversations remain readable.
            Administrators can remove individual messages: a removed message is replaced by a
            marker saying who removed it, and its text, and any links it carried, are removed.
          </li>
          <li>
            <strong>Links sent to you</strong> stay in your list until you dismiss them, or until
            the sender's account is deleted. Links attached to a chat message stay with it, since
            chat is kept permanently; once a link expires the message says so instead of offering
            it.
          </li>
          <li>
            <strong>Your profile</strong> stays until you change it or your account is deleted.
            A picture you replace or remove is deleted at once; a deleted account's picture goes
            at the next cleanup, normally within {minutes(r.sweep_minutes)}.
          </li>
          <li>
            <strong>Your account</strong> lasts until an administrator deletes it; there is no way
            to delete your own. Deleting an account removes it, its sessions and its links, whose
            files are then deleted as above. Its chat messages remain, as above.
          </li>
          <li>
            <strong>Backups</strong> of the database — accounts and profiles, links and chat, but
            not the files or pictures —
            are made every day and before every update, and each is deleted after
            {days(r.backup_days)}. So something deleted from the site can survive in a backup for
            up to {days(r.backup_days)}.
          </li>
          <li>
            <strong>Logs</strong> are rotated, oldest first, once they reach a fixed size, and are
            also cleared whenever the site is updated.
          </li>
        </ul>
      </section>

      <section>
        <h2>What is not done</h2>
        <p>
          No analytics, no tracking, no advertising, and no third-party scripts, fonts or
          embeds: everything a page here loads comes from this server, and the site's content
          security policy refuses anything else. Nothing is sold or passed on to anyone.
        </p>
      </section>

      <section>
        <h2>Why</h2>
        <p>
          Everything above is kept to provide the service you joined — sharing files and talking
          about them — and to keep it secure. Where data-protection law such as the GDPR applies,
          that is its basis.
        </p>
      </section>

      <section>
        <h2>Your rights</h2>
        <p>
          You can ask for a copy of what is stored about you, for it to be corrected, or for it
          to be deleted{#if info.operator?.contact}, by emailing
            <a href="mailto:{info.operator.contact}">{info.operator.contact}</a>{:else}, by asking
            the person who runs the site{/if}. If something shared here shows you and you are not a
          member, you can ask for it to be removed the same way. If you are in the EU or EEA, you
          can also complain to your national data protection authority.
        </p>
      </section>
    </div>
  {/if}
</div>

<style>
  /* Long-form text is rare in this app, so the notice styles its own elements rather
     than adding a typography plugin for one page. */
  .notice :global(h2) {
    margin-bottom: 0.5rem;
    font-size: 0.875rem;
    font-weight: 500;
    color: var(--color-ink-100);
  }
  .notice :global(p + p) {
    margin-top: 0.75rem;
  }
  .notice :global(ul) {
    display: grid;
    gap: 0.75rem;
    padding-left: 1rem;
    list-style: disc;
  }
  .notice :global(li::marker) {
    color: var(--color-ink-700);
  }
  .notice :global(strong) {
    font-weight: 500;
    color: var(--color-ink-100);
  }
  .notice :global(a) {
    color: var(--color-accent);
    text-decoration: underline;
    text-underline-offset: 2px;
  }
</style>
