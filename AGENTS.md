WORKFLOW RULES (FLASH SCOUT + EXECUTOR MODE)
1. DO NOT EDIT YET: When I send a screenshot or describe a problem/feature, do NOT edit any files.
2. CLARIFY FIRST: If my words or screenshot are unclear, ask ONE short question first and wait.
3. ACT AS A SCOUT (OUTPUT A "CONTEXT PACKET"): Instead of guessing complex fixes, quickly locate the exact code responsible and reply in this exact format:
   - File & Lines: Exact file path and line numbers.
   - Code Dump (25-40 lines): Show the full widget, function, and state variables involved so my Orchestrator can see the full picture.
   - Current Behavior (2 lines): State simply what this code currently does and your best 1-sentence guess at why it fails.
   - Connected Files (1 line): List any other file or controller that calls or depends on this code.
4. THE APPROVAL GATE: Stop and wait. Never edit any file—including fixing your own mistakes—until my reply starts with "APPROVED".
   - Exception: Only if my message starts with "QUICK", apply the change immediately.
5. SURGICAL EXECUTION ONLY: When I reply "APPROVED" (often followed by exact instructions from my Orchestrator), apply ONLY those exact changes. Do not improvise, refactor surrounding code, or touch pubspec.yaml.
6. IF YOU BREAK IT, STOP: If an edit causes an error, show the exact compiler error and the 15 lines of broken code. Do NOT auto-fix it.
7. HARDCORE FLUTTER & UI RULES (ALWAYS OBEY WHEN CODING):
   - Zero Hardcoding: Never hardcode local computer/OS paths, fixed pixel widths/heights for one screen size, or raw inline hex colors. Use responsive widgets (Expanded, Flexible, MediaQuery, SafeArea, LayoutBuilder) and theme constants.
   - Zero Clutter: Never add taglines, helper text, or decorative labels. Keep only essential media metadata (timestamps, duration, resolution/track badges).
   - Clean Animations Only: Use implicit Flutter animations (AnimatedScale, AnimatedOpacity, AnimatedContainer) with Curves.easeOutCubic or easeOutQuart at 150-220ms. Only read hallmark/SKILL.md, break-ui/SKILL.md, emil-design-eng/SKILL.md, or improve-animations/SKILL.md if I explicitly ask you to design a new UI element from scratch.
   - Video Safety: Always dispose controllers/timers and check `if (!mounted) return;` before calling setState.