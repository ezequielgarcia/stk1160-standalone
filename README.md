stk1160-standalone
==================

stk1160 release 0.9.3\_for\_v3.4

This driver is specially made for you: the user;
so anything you think it's not clear enough, please open an github issue.

This is the stk1160 driver (formerly known as easycap driver).
It's the driver needed to capture audio and video in some of those
little devices named as Easycap (if they are based on stk1160 chip).

You need to have this modules installed, but don't worry,
you don't have to insert them manually:

- snd\_usb\_audio
- snd\_ac97\_codec
- ac97\_bus
- saa7115
- videodev
- videobuf2\_core
- v4l2\_common

Building and installing
-----------------------

    make
    make install

Piece of cake, uh?
