---
name: draft-post
description: Draft a build-in-public post for FloodMesh from the repo's commits, docs and test notes. Writes X, Instagram and (for announcements) LinkedIn variants into social/drafts/. Use when asked to draft a post, write a thread, write a reel script, or when a topic from social/backlog.md is due.
argument-hint: [topic or backlog number, or blank for the next queued topic]
---

# Draft a FloodMesh post

Write one draft file for one topic. Do not post anything anywhere.

## Steps

1. Read `social/README.md` for the format, footer and rules. Follow them
   exactly. Read `social/backlog.md` for the queue.
2. Pick the topic: the argument if given, otherwise the first Queue row with
   an empty Draft column whose Material exists in the repo. If the argument
   is `weekly`, skip the backlog: write the LinkedIn weekly update for the
   last seven days to `social/drafts/YYYY-MM-DD-linkedin-weekly.md`,
   following the "LinkedIn weekly update" section of `social/README.md`
   exactly, then go to step 6.
3. Gather material. Run `git log --since="14 days ago" --stat` and read the
   files the topic's Material column names. Read the Status and Known work
   remaining sections of `README.md`. Everything stated in the post must
   trace to one of these. If the material is missing (a test not yet run, a
   feature not yet built), skip that row and say why in the reply.
4. Write `social/drafts/YYYY-MM-<slug>.md` with:
   - Header: topic, tags, channels, and a Visual line saying what photo or
     clip to shoot, from the real build only.
   - A Facts block listing each number or claim and where it came from.
   - X: a single post under 280 characters, or a numbered thread with one
     idea per post, the result or hook in post 1, footer in the last post.
   - Instagram: caption with the hook in the first 125 characters, footer,
     then 5 to 10 hashtags on the final line. For #trials and build topics,
     add a reel script with timestamps, on-screen text, and the result in
     the first two seconds.
   - LinkedIn: only for #announcement, #pilot or #collaboration topics.
     Hook first, who you are asking and what for, three tags at the end.
5. Fill the Draft column for that row in `social/backlog.md`.
6. Reply with the file path and the full draft text so it can be read
   without opening the file.

## Voice

First person, written for a neighbour who has never heard of LoRa. Short
sentences, short common words. Explain a technical term only if the post
cannot work without it, and only once; otherwise leave it out. Features and
milestones, never internals (README rule 6). Say why the work matters and
that you care, in plain words. Stay humble: say what is tested and what is
not, what failed, and what you still do not know. One question per post,
about what people need or would change, never about design choices. No
metaphors, no drama, no poetic lines, nothing that sounds written by a
machine. No hype words, no "revolutionary", no "game-changing". No
em-dashes.

## Never

- Claim a range, battery life, hop count or waterproof rating that has not
  been measured on hardware.
- Mention WPC certification or type approval as done or imminent.
- Invent quotes from users, officials or testers.
- Name chips, modules, part numbers, pins, radio settings, packet sizes or
  protocol details in post text.
- Dress up a result with a metaphor or a dramatic line to make it sound
  bigger or sadder than it is.
- Generate images for test results. Rendered cards are only for
  #announcement posts.
