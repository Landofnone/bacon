.. currentmodule:: bacon

Events
------

You handle events by overriding methods in the :class:`Game` class.  Typically your game will be structured with a instance of a
subclass of :class:`Game`, which overrides all the event methods you wish to handle.

Bacon also stores the state of all input devices; so for example, you can query whether a particular key is pressed or not at any
point in your program, without having to handle the event.

Game events
===========

Before any frame tick events but after the graphics device is initialized, the game receives a 
single call to :func:`Game.on_init`.  Overriding this is not usually necessary as most initialization
can be done outside of the Game class.  However, the event exists for convenience or to initialize
offscreen targets.

.. note:: TODO on_init example

Every frame Bacon sends the :func:`Game.on_tick` event.  You must override this method, as it is the only way to update and render
the game.

In order to keep animations playing back at a constant speed independent of the framerate, refer to
:data:`timestep`:

.. note:: TODO timestep example

Keyboard
========

The :func:`Game.on_key` function is called when a key is pressed or released:

.. literalinclude:: ../examples/key_event.py

Alternatively, you can check the :data:`keys` set at any time to determine if a key is currently pressed.  The following example clears
the screen to green as long as the spacebar is pressed:

.. literalinclude:: ../examples/keys.py

See the :doc:`keys` for a complete list of supported key codes.

Mouse
=====

The :func:`Game.on_mouse_button` function is called when a mouse button is pressed or released:

.. literalinclude:: ../examples/mouse_button_event.py

The :func:`Game.on_mouse_scroll` function is called whenever the mouse wheel is scrolled:

.. literalinclude:: ../examples/mouse_scroll_event.py

The current position of the mouse, and the state of its buttons, can be queried from the :data:`bacon.mouse`
object:

.. literalinclude:: ../examples/mouse.py

Game Controllers
================

To support the use of game controllers, first listen for the :func:`Game.on_controller_connected` event.  Check the passed 
in :class:`Controller` instance for the set of buttons and axes it supports.  Then either listen for 
:func:`Game.on_controller_button` and :func:`Game.on_controller_axis` events, or check the state of the controller's
buttons and axes on the controller instance itself.

.. literalinclude:: ../examples/controller.py

Game Controller Button and Axis Mapping
=======================================

Unfortunately there is no prescribed standard for game controllers when it comes to the naming of their buttons, thumbsticks and
triggers.  Bacon attempts to unify all game controllers into one of a small number of predefined 
:class:`ControllerProfile`s, for example ``ControllerProfile.extended``.

Developing a game that can use the *generic* profile is quite difficult, and usually requires a setup step where the user maps
each button and axis on their controller to a game action.  Using the *standard* or *extended* profiles is easy, however; once
you've checked that the profiler matches the required profile, start reading the named inputs (such as :attr:`Controller.dpad_left`).

In order to support these profiles, a :class:`ControllerMapping` must be provided that maps between the generic inputs (such as :attr:`ControllerButtons.button1`) to the profile inputs.  Bacon is distributed with some well-known controllers such as the Xbox 360 controller
mappings built-in, but you can also provide your own.

To supply a new mapping, call :func:`ControllerMapping.register` with the vendor an product IDs of the controller you are describing,
and a :class:`ControllerMapping` that describes the mapping.  For example, here is the built-in mapping for the Xbox 360 controller
used on OS X:

.. literalinclude:: ../bacon/controller_profiles.py
	:lines: 4-36
