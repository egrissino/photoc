/* Binary assets linked into the program, so it runs without files beside it: the Inter and
   JetBrains Mono fonts (OFL, see assets/fonts) and the app icon. */
#ifdef _WIN32
    .section .rdata,"dr"
#else
    .section .rodata
#endif
    .balign 16
    .global pc_font_inter_regular
    .global pc_font_inter_regular_end
pc_font_inter_regular:
    .incbin "assets/fonts/Inter-Regular.ttf"
pc_font_inter_regular_end:
    .balign 16
    .global pc_font_inter_medium
    .global pc_font_inter_medium_end
pc_font_inter_medium:
    .incbin "assets/fonts/Inter-Medium.ttf"
pc_font_inter_medium_end:
    .balign 16
    .global pc_font_inter_semibold
    .global pc_font_inter_semibold_end
pc_font_inter_semibold:
    .incbin "assets/fonts/Inter-SemiBold.ttf"
pc_font_inter_semibold_end:
    .balign 16
    .global pc_font_mono
    .global pc_font_mono_end
pc_font_mono:
    .incbin "assets/fonts/JetBrainsMono-Regular.ttf"
pc_font_mono_end:
    .balign 16
    .global pc_app_icon
    .global pc_app_icon_end
pc_app_icon:
    .incbin "assets/photoc.png"
pc_app_icon_end:
#ifdef __ELF__
    .section .note.GNU-stack,"",@progbits
#endif
