from .InputControlElement import InputControlElement
class ButtonElement(InputControlElement):
    def __init__(self, momentary, t, ch, cc): InputControlElement.__init__(self, t, ch, cc)
