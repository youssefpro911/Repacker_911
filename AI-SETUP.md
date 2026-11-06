# RPK AI Advisor — Offline setup

RPK AI Advisor runs a local GGUF model through the llama.cpp command-line runtime. It does not use Ollama or a cloud API. The runtime and model are separate downloads, so the normal RPK application package stays small.

## Download the llama.cpp runtime

Open the official llama.cpp Releases page:

https://github.com/ggml-org/llama.cpp/releases

Download a Windows x64 build suitable for your PC, extract it, and locate \`llama-cli.exe\`. In RPK AI Advisor, use **Browse** beside the runtime field and select that executable. The runtime release may be CPU-only, CUDA, or Vulkan; choose the variant appropriate for your system.

## Download a GGUF model

A recommended starting point is **Qwen2.5-Coder-7B-Instruct-GGUF**, using the **Q4_K_M** quantization. The model page lists that file at approximately 4.68 GB:

https://huggingface.co/Qwen/Qwen2.5-Coder-7B-Instruct-GGUF

Download the Q4_K_M GGUF file and select it in the Model field of RPK AI Advisor. You can use a different compatible GGUF model too.

## Ask the local assistant

1. Open \`rpk_ai.exe\`, or click **AI Assistant** in the main RPK window.
2. Select \`llama-cli.exe\` and your \`.gguf\` model.
3. Enter an RPK question or paste an error log.
4. CPU inference is selected by default. Enable GPU acceleration only if the selected llama.cpp build has matching GPU support and sufficient VRAM.
5. Click **Ask local model**.

The app remembers the runtime and model paths in \`%LOCALAPPDATA%\\RPK\\ai.ini\`. Model prompts run locally; RPK does not upload the prompt to a remote service.

The assistant is an advisor only. It does not automatically change compression settings, edit archives, or execute the code it suggests. Verify configuration changes with normal archive tests.
