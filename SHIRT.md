# The Official fizzfest Shirt 🥤

There is no merch warehouse. There is no fulfillment center. There is this
file and your local print shop. That is the open source way: we provide the
party, you provide the cotton.

## Front print

Black shirt, white and pink print. Big text on the chest, small text under
it. The bubbles are optional but strongly encouraged.

```
      O  o   o  O
    o            o
   O   FIZZFEST   O
    o    2 0 2 6 o
     O  o   o  O
       `---┬---'
     .---------.
     |.-------.|
     | >  🥤  < |
     |'-------'|
     '---------'
```

Main lockup:

- Line 1: `FIZZFEST` (bold, white, letterspaced)
- Line 2: `i % 15 ? "fizzbuzz" : i % 3 ? "fizz" : i % 5 ? "buzz" : i` (monospace, pink #FF4FD8, small)
- Bubbles: three or four hollow circles drifting up the left side, pink

## Back print (optional, for the true believers)

- Line 1: `I FIZZED IN 2026`
- Line 2: `github.com/ssmurfgg04-gif/fizzfest` (small, white)

## Print-ready SVG

Open this in any browser or design tool, scale to A4, take it to the shop:

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="600" height="700" viewBox="0 0 600 700">
  <rect width="600" height="700" fill="none"/>
  <text x="300" y="300" font-family="monospace" font-size="72" font-weight="bold"
        fill="#FFFFFF" text-anchor="middle" letter-spacing="8">FIZZFEST</text>
  <text x="300" y="360" font-family="monospace" font-size="20"
        fill="#FF4FD8" text-anchor="middle">i % 15 ? "fizzbuzz" : i % 3 ? "fizz" : i % 5 ? "buzz" : i</text>
  <circle cx="150" cy="150" r="24" fill="none" stroke="#FF4FD8" stroke-width="4"/>
  <circle cx="205" cy="105" r="14" fill="none" stroke="#FF4FD8" stroke-width="3"/>
  <circle cx="450" cy="170" r="18" fill="none" stroke="#FF4FD8" stroke-width="4"/>
  <circle cx="415" cy="115" r="10" fill="none" stroke="#FF4FD8" stroke-width="3"/>
  <text x="300" y="620" font-family="monospace" font-size="16"
        fill="#FFFFFF" text-anchor="middle">I FIZZED IN 2026</text>
</svg>
```

## The fine print

- Print it on anything: shirt, hoodie, mug, poster, your laptop
- Change the colors if pink is not your thing, the party will not mind
- Wear it to a meetup and find the other fizzers. This is the whole
  marketing strategy
