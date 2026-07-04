PUBLIC SuitDamageHookDetourAsm
PUBLIC MasterCoordinateHookAsm

EXTERN SuitDamageLogic : PROC
EXTERN g_return_address : QWORD
EXTERN g_player_base: QWORD
EXTERN g_coord_return: QWORD

.code

; --------------------------------------------------
; HOOK 1: SUIT DAMAGE DETOUR
; --------------------------------------------------
SuitDamageHookDetourAsm PROC
    ; 1. Save standard registers
    push rax
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    
    ; 2. Protect the XMM graphics registers from C++
    sub rsp, 48h
    movdqu [rsp], xmm1
    movdqu [rsp+10h], xmm2
    movdqu [rsp+20h], xmm3

    ; 3. Setup arguments for C++
    mov rcx, rax       ; Argument 1: The suit pointer
    movss xmm1, xmm0   ; Argument 2: The game's intended suit float

    ; 4. ALIGN THE STACK TO 16 BYTES (The Crash Fix)
    push rbp           ; Save RBP (non-volatile register)
    mov rbp, rsp       ; Backup the exact unaligned stack pointer
    and rsp, -16       ; Force 16-byte alignment (0xFFFFFFFFFFFFFFF0)
    sub rsp, 20h       ; Allocate 32 bytes of shadow space for C++

    ; 5. Call our C++ logic safely
    call SuitDamageLogic

    ; 6. RESTORE THE UNALIGNED STACK
    mov rsp, rbp       ; Restore the exact stack pointer
    pop rbp            ; Restore RBP

    ; 7. Restore the XMM graphics registers perfectly
    movdqu xmm1, [rsp]
    movdqu xmm2, [rsp+10h]
    movdqu xmm3, [rsp+20h]
    add rsp, 48h

    ; 8. Restore standard registers
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx
    pop rax

    ; 9. Execute original AVX instruction + NOP
    DB 0C5h, 0F8h, 011h, 000h, 090h

    ; 10. Jump back into the game safely
    jmp qword ptr [g_return_address]

SuitDamageHookDetourAsm ENDP

; --- 2. THE MASTER COORDINATE STEALER ---
MasterCoordinateHookAsm PROC
    ; 1. Steal RCX (The base pointer for the absolute global coordinates)
    mov g_player_base, rcx
    
    ; 2. Execute the original 5-byte Z-coordinate write
    ; vmovss [rcx+3Ch],xmm2
    DB 0C5h, 0FAh, 011h, 051h, 03Ch
    
    ; 3. Jump back to the game seamlessly
    jmp qword ptr [g_coord_return]
MasterCoordinateHookAsm ENDP

END