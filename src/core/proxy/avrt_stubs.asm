; avrt.dll proxy jump stubs: each export jumps to the System32 function (target table filled in DllMain).
EXTERN g_avrt:QWORD
.code
AvCreateTaskIndex PROC
    jmp QWORD PTR [g_avrt + 0]
AvCreateTaskIndex ENDP
AvQuerySystemResponsiveness PROC
    jmp QWORD PTR [g_avrt + 8]
AvQuerySystemResponsiveness ENDP
AvQueryTaskIndexValue PROC
    jmp QWORD PTR [g_avrt + 16]
AvQueryTaskIndexValue ENDP
AvRevertMmThreadCharacteristics PROC
    jmp QWORD PTR [g_avrt + 24]
AvRevertMmThreadCharacteristics ENDP
AvRtCreateThreadOrderingGroup PROC
    jmp QWORD PTR [g_avrt + 32]
AvRtCreateThreadOrderingGroup ENDP
AvRtCreateThreadOrderingGroupExA PROC
    jmp QWORD PTR [g_avrt + 40]
AvRtCreateThreadOrderingGroupExA ENDP
AvRtCreateThreadOrderingGroupExW PROC
    jmp QWORD PTR [g_avrt + 48]
AvRtCreateThreadOrderingGroupExW ENDP
AvRtDeleteThreadOrderingGroup PROC
    jmp QWORD PTR [g_avrt + 56]
AvRtDeleteThreadOrderingGroup ENDP
AvRtJoinThreadOrderingGroup PROC
    jmp QWORD PTR [g_avrt + 64]
AvRtJoinThreadOrderingGroup ENDP
AvRtLeaveThreadOrderingGroup PROC
    jmp QWORD PTR [g_avrt + 72]
AvRtLeaveThreadOrderingGroup ENDP
AvRtWaitOnThreadOrderingGroup PROC
    jmp QWORD PTR [g_avrt + 80]
AvRtWaitOnThreadOrderingGroup ENDP
AvSetMmMaxThreadCharacteristicsA PROC
    jmp QWORD PTR [g_avrt + 88]
AvSetMmMaxThreadCharacteristicsA ENDP
AvSetMmMaxThreadCharacteristicsW PROC
    jmp QWORD PTR [g_avrt + 96]
AvSetMmMaxThreadCharacteristicsW ENDP
AvSetMmThreadCharacteristicsA PROC
    jmp QWORD PTR [g_avrt + 104]
AvSetMmThreadCharacteristicsA ENDP
AvSetMmThreadCharacteristicsW PROC
    jmp QWORD PTR [g_avrt + 112]
AvSetMmThreadCharacteristicsW ENDP
AvSetMmThreadPriority PROC
    jmp QWORD PTR [g_avrt + 120]
AvSetMmThreadPriority ENDP
AvSetMultimediaMode PROC
    jmp QWORD PTR [g_avrt + 128]
AvSetMultimediaMode ENDP
AvTaskIndexYield PROC
    jmp QWORD PTR [g_avrt + 136]
AvTaskIndexYield ENDP
AvTaskIndexYieldCancel PROC
    jmp QWORD PTR [g_avrt + 144]
AvTaskIndexYieldCancel ENDP
AvThreadOpenTaskIndex PROC
    jmp QWORD PTR [g_avrt + 152]
AvThreadOpenTaskIndex ENDP
END
