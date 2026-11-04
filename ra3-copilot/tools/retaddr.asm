; _ReturnAddress() -> clang emits a call to __ReturnAddress on i686-w64-mingw32;
; mingw has no such runtime helper.  Return the caller's return address.
.386
.model flat, C
.code
__ReturnAddress PROC
    mov eax, [esp]
    ret
__ReturnAddress ENDP
END
