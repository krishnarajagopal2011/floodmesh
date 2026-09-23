# FloodMesh in public

Channel plan, post format, and the rules every draft follows.
Drafts live in `drafts/`, the topic queue in `backlog.md`.
The `/draft-post` skill writes new drafts from the repo's commits and docs.
A weekly cloud routine runs it and emails the result to the person who
queues posts in Buffer.

## Who does what

- Krishna builds and tests. Adds topics to `backlog.md` when something
  happens. Answers replies on X and Instagram.
- The routine drafts one or two posts a week from the repo and emails them.
- The partner edits, shoots or picks the visual, queues in Buffer, and moves
  the row to Done in `backlog.md` with the date.

## Channels

| Channel | Role | Cadence | Format |
|---|---|---|---|
| GitHub | Home. Every post links here. | Continuous | README, Discussions, test logs |
| X | Main feed. Makers, LoRa/Meshtastic people, disaster-tech accounts. | 2 to 3 per week | Single post or short thread, 1 photo or clip |
| Instagram | Visual feed. Reels of tests and builds, carousels of photos. | 1 reel per week, carousels when photos exist | 9:16 vertical, 15 to 45 s, captions burned in |
| LinkedIn | Announcements and asks only. Officials, NGOs, CSR funders, vendors. | 2 per month | Longer text, first two lines carry the hook |
| Reddit | Two specific asks only: 3D printing help, pilot testers. | As needed | Plain text, no marketing voice |

## Post format

Every post starts with a tag, then a one-line "We are building" hook,
then the body, then the fixed footer.

```
#tag #tag

We are building an off-grid pager for floods. <one line on what this post is>

<body>

<footer>
```

Tags in use: `#announcement` `#discussion` `#feedback` `#trials` `#pilot`
`#collaboration` `#resources` `#cheers` `#story`

### Footer (fixed, three lines)

```
Why: in Chennai 2015 and during Cyclone Michaung, towers and power failed together. Neighbours 200 m apart could not reach each other.
FloodMesh hops alarms and 10-second voice notes around buildings, for days on common lithium cells.
Open hardware, built in public: github.com/krishnarajagopal2011/floodmesh
```

On X the footer goes in the last post of a thread, or is cut to the third
line only on a single post.

## Per-platform rules

**X.** 280 characters per post. Hashtags cost characters and do little, so
keep the two leading tags and no more. Threads: one idea per post, photo on
the first post. Numbers and results go in the first post.

**Instagram.** Caption up to 2,200 characters. The first 125 characters show
before "more", so the hook goes there. Put 5 to 10 hashtags at the end of the
caption, not in the body. Reels: vertical, burned-in captions since most
people watch muted, the result stated in the first two seconds. Real footage
only. Rendered graphics are for announcement cards, never for test results.

**LinkedIn.** No tag at the very top. Open with the hook line, tags at the
end, three at most. Say who you are asking for and what you want from them.

## Rules for every draft

1. Only claims the repo can back. Status, numbers and dates come from
   `README.md`, `docs/`, and test notes. If it is not verified on hardware,
   the post says so.
2. No regulatory claims. Type approval and WPC certification are roadmap
   items. Do not announce them until the paperwork exists.
3. Real photos and clips of the actual build. Ugly perfboard is the point.
4. Ask one question per post. Two questions get zero answers. Ask about
   features and use (what people need, what they would change), never about
   circuit or design choices.
5. Reply to every comment within a day. That is where the pilot testers
   come from.
6. Talk about features and milestones, not internals. Never name chips,
   modules, part numbers, pins, memory sizes, radio settings, packet sizes
   or protocol details, and no hashtags for them. "Our own circuit board is
   designed" is fine; what is on it is not. Visuals show the device or the
   whole board, never labelled parts or schematic pages. The Facts block may
   cite technical files; the post text may not repeat them.

## Publishing

Buffer covers X, Instagram and LinkedIn from one queue. Instagram needs a
creator or business account for scheduled Reels. Load a week at a time.
Each emailed draft names the visual to shoot. If the visual does not exist
yet, hold the post rather than using a rendered image.
