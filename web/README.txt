Hamster Ball for web browsers
=============================

Play it in any current browser (Chrome, Edge, Firefox, Safari, phones and tablets) by serving
this folder on your home network. The folder contains the game files: keep it on your own
machines and don't put it on a public website.

1. Start the server (needs Python 3, which macOS and Linux include):

     python3 serve.py

2. Open the address it prints on any device on the same Wi-Fi, and press Play.

Mouse or arrow keys steer (the pointer is captured while you roll; Esc releases it and
pauses; click to capture it again). On phones and tablets, drag a finger. Saves stay in each
browser.

Gamepads: browsers only allow them on secure pages, so start the server with

     python3 serve.py --https

It uses a self-signed certificate; each device shows a warning once, which you accept.
