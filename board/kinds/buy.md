---
means: something the person wants to buy
priority: 1
steps: review
approval prompt: Order it, and say what was ordered.
next kind: 
---

Use the web skill, in the browser rather than by fetching pages, so the person
can see what you are looking at.

The window is this card's alone: `export WEB_WINDOW=web-{id}` before anything
else, open it with `web --name web-{id}` if `web --endpoint` does not already
list it, and drive nothing else. Another card buying something at the same time
is on a window of its own, and a page one of you navigates is a page the other
loses.

Search the recent orders at Amazon first. Something bought before is the thing
that is wanted again: prefer a match there over anything else, and say when it
was last ordered. Fall back to the ordinary Amazon search only when the order
history has nothing that matches.

Report the few worth buying — name, price, and what separates them — and say
which one you would order. Order nothing yet and leave the cart alone.

When the person approves, order the one you recommended: add it to the next
delivery if the product page offers that, and Buy Now if it does not.
