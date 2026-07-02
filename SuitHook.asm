PUBLIC SuitDamageHookDetourAsm
EXTERN SuitDamageLogic : PROC
EXTERN g_return_address : QWORD

.code
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

    ; 3. Call our C++ logic (requires 28h shadow space for Windows x64)
    mov rcx, rax
    sub rsp, 28h 
    call SuitDamageLogic
    add rsp, 28h

    ; 4. Restore the XMM graphics registers perfectly
    movdqu xmm1, [rsp]
    movdqu xmm2, [rsp+10h]
    movdqu xmm3, [rsp+20h]
    add rsp, 48h

    ; 5. Restore standard registers
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx
    pop rax

    ; 6. Execute original AVX instruction + NOP
    DB 0C5h, 0F8h, 011h, 000h, 090h

    ; 7. Jump back into the game safely
    jmp qword ptr [g_return_address]

SuitDamageHookDetourAsm ENDP
END