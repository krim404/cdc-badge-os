# CDC Views - Reusable Standard View Components
# Contains ListView, T9Input, Slider, PinEntry, DateInput, TimeInput, etc.

idf_component_register(
    SRCS
        "src/ListView.cpp"
        "src/SliderView.cpp"
        "src/PinEntryView.cpp"
        "src/DateInputView.cpp"
        "src/TimeInputView.cpp"
        "src/MessageBox.cpp"
        "src/T9InputView.cpp"
        "src/PasswordT9View.cpp"
        "src/InfoView.cpp"
        "src/MarkdownParser.cpp"
        "src/MarkdownView.cpp"
        "src/ImageView.cpp"
        "src/ToastView.cpp"
        "src/ContextMenuView.cpp"
        "src/QRCodeView.cpp"
        "src/ConfirmView.cpp"
        "src/RenderHelpers.cpp"
        "src/HtmlViewerHook.cpp"
        "src/CanvasView.cpp"
        "src/ColorPickerView.cpp"
        "src/Fonts.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        cdc_ui
        cdc_core
        cdc_hal
        cdc_log
        CalEPD
        Adafruit-GFX
        cdc_image
)
