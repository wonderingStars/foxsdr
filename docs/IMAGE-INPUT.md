# Plugin image input (0.99.81)

An image dashboard can declare `CASCADE_CAP_IMAGE_INPUT` alongside
`CASCADE_CAP_IMAGE_DECODER`, supplying `CascadeImageInputApi`. The existing
decoder table and ABI version remain unchanged, so older hosts can continue
using the image decoder and ignore its optional input capability.

The host calls `press(x,y)` on its GUI thread after a left press and release
inside the displayed picture. Coordinates are native pixels with the origin at
the top left, even when the picture is scaled to fit its window. A gesture
starting elsewhere or released outside the picture does not call the plugin.
The callback targets the plugin's primary image session. It runs under the
runner lock shared by image decoder callbacks and lifecycle operations and
must return promptly; queue decoder work when necessary.

The host validates the table size and callback and requires an image decoder.
Input is ignored before the first valid picture, outside its bounds, after
decoder failure, and after the decoder stops. The GUI displays a hand cursor
only for an interactive image. Plugins should show clickable-control wording
only when `get_state` includes `CASCADE_STATE_IMAGE_INPUT`.

IBOC Radio's updated plugin uses this capability to make the entire HD programme
row selectable, including its arrow, HD label and programme name. Both the
0.99.81 host and updated IBOC plugin are required for dashboard clicks.

The same host update keeps automatic decoder muting confined to the receiver's
demodulated audio before plugin audio takes over. Decoded DAB audio remains
audible, while user and transmit mute still silence the final output.
