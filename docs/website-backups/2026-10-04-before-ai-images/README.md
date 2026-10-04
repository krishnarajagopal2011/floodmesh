# Website backup: before AI-generated images (4 Oct 2026)

A copy of the public website as it was reviewed on 4 October 2026, before
any Hugging Face images were added: the hero section (the illustrated unit
playing a neighbour's message, a reply and an SOS) and the animated street
map in "How it works" (`assets/js/mesh.js`).

Source commit: `6e68615` on `9th-Sept-2026`.

## To go back to this version

The owner asked for this so the site can be reverted if the AI images aren't
good enough. Either:

- restore the whole site from the commit (keeps later fixes out):

      git checkout 6e68615 -- website/index.html website/assets/css/site.css website/assets/js/site.js website/assets/js/mesh.js website/assets/js/i18n.js
      git commit -m "website: revert to the hero and street map from before the AI images"

- or copy the files in this folder back over `website/` (same result for
  these files).

Open `index.html` here in a browser to see this version locally (the order
form shows a preview result; nothing is sent).
