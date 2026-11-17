Master server (masterserver.dankelsoft.dk): 007 NightFire, Battlefield 1943 and Bad Company PC

Games are listed from GAMES in api.php (nightfire, bf1943, bfbc1): keep every one when adding or changing a game.
A server whose beacon answers with the "lock" field (a password) is listed with locked=1 (the lines format: an extra 9th field).
A beacon answering "hardcore" is listed with hardcore=1 (the lines format: a 10th field); the website and the game browsers tag it HARDCORE.

Upload the two files to the ROOT of the site (public_html), keeping them together:
  api.php    the server list API (announce, list, remove, games)
  index.html the website

The folder must be writable by PHP (the cPanel default). servers.json is created by the API.
Check it works: open https://masterserver.dankelsoft.dk/api.php?a=selftest
It should say "ok": true. If can_send_udp is false, the host blocks outgoing UDP.
