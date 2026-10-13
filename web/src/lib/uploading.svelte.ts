// How many uploads are in flight, so that leaving the page can warn first.
//
// A tus upload resumes only from the browser that still holds the file. Close the tab,
// reload or sign out mid-transfer and what was sent is stranded: the file has to be
// chosen and sent again from the start. While anything is in flight, the browser's own
// "Leave site?" prompt guards reload, close and navigating away, and sign-out asks first.
//
// Counted from the upload callbacks rather than derived from the Uploads page's list,
// because an upload keeps going when that page is unmounted — on the account page, say —
// and the guard has to keep holding until it ends.

class UploadActivity {
  active = $state(0)

  start() {
    if (this.active++ === 0) window.addEventListener('beforeunload', guard)
  }

  end() {
    if (this.active === 0) return
    if (--this.active === 0) window.removeEventListener('beforeunload', guard)
  }
}

// Registered only while needed: a standing beforeunload handler would also keep the page
// out of the back/forward cache for no reason.
function guard(event: BeforeUnloadEvent) {
  event.preventDefault()
  // Older browsers need this set to show the prompt; its text is not shown by any.
  event.returnValue = ''
}

export const uploading = new UploadActivity()
