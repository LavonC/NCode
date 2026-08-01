#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <sstream>
#include <thread>
#include <condition_variable>
#include <llama.h>
#include <llama-cpp.h>
#include <cmath>

struct hitBox {
    std::string name;
    SDL_FRect box;
};

struct RenderLine {
    TTF_Text* numText = nullptr;
    TTF_Text* contentText = nullptr;
    int numW = 0;
    int numH = 0;
    int contentW = 0;
    int contentH = 0;
};

struct Folder {
    std::string name;
    std::string path;
    bool isExpanded = false;
    std::vector<Folder*> childFolders;
    std::vector<std::string> files;

    ~Folder() {
        for (Folder* f : childFolders) {
            delete f;
        }
        childFolders.clear();
    }
};

struct SideBarItem {
    bool isFolder;
    Folder* folderPtr;
    std::string name;
    std::string path;
    int depth;
    TTF_Text* textRender = nullptr;
    int w = 0;
    int h = 0;
};

class Editor {
private:
    bool _state = false;
    bool _change = true;
    SDL_Event _event;
    SDL_Window* _window = nullptr;
    SDL_Renderer* _renderer = nullptr;
    int _width = 800;
    int _height = 600;
    TTF_Font* _font = nullptr;
    
    std::vector<std::string> _fileContent;
    std::vector<RenderLine> _renderLines;
    
    SDL_FRect _textArea;
    SDL_FRect _titleBar;
    SDL_FRect _sideBar;
    TTF_TextEngine* _textEngine = nullptr;
    std::vector<hitBox> _hitBoxes;
    
    std::atomic<bool> _isFileReady{false};
    std::atomic<bool> _isSaveReady{false};
    std::mutex _fileMutex;              
    
    std::string _pendingFilePath;
    std::string _pendingSavePath;
    std::string _currentFilePath = ""; 
    
    int _textHeight = 0;
    int _textWidth = 0;
    int _charWidth = 0; 

    float _scrollOffsetY = 0.0f;
    float _scrollOffsetX = 0.0f;
    const float SCROLLBAR_SIZE = 15.0f;

    SDL_FRect _vThumb = {0, 0, 0, 0};
    SDL_FRect _hThumb = {0, 0, 0, 0};
    bool _isDraggingV = false;
    bool _isDraggingH = false;
    float _dragOffsetY = 0.0f;
    float _dragOffsetX = 0.0f;

    bool _isSelectingText = false;
    int _selStartLine = -1;
    int _selStartCol = 0;
    int _selEndLine = -1;
    int _selEndCol = 0;

    int _cursorLine = 0;
    int _cursorCol = 0;

    bool _isDirty = false;
    std::atomic<bool> _isFolderReady{false};
    std::mutex _folderMutex;
    std::string _pendingFolderPath;
    std::string _currentFolderPath = "";

    Folder* _rootFolder = nullptr;
    std::vector<SideBarItem> _sidebarItems;
    
    float _sideScrollOffsetY = 0.0f;
    float _sideScrollOffsetX = 0.0f;
    SDL_FRect _sideVThumb = {0, 0, 0, 0};
    SDL_FRect _sideHThumb = {0, 0, 0, 0};
    bool _isDraggingSideV = false;
    bool _isDraggingSideH = false;
    float _dragSideOffsetY = 0.0f;
    float _dragSideOffsetX = 0.0f;
    int _sideTextHeight = 0;
    int _sideTextWidth = 0;

    bool _aiEnabled = false;
    std::thread _aiThread;
    std::atomic<bool> _aiThreadRunning{true};
    std::condition_variable _aiCv;
    std::mutex _aiWorkerMutex;
    std::atomic<bool> _pendingAiRequest{false};
    std::atomic<bool> _cancelGeneration{false};
    
    std::string _aiPrefix;
    std::string _aiSuffix;
    int _aiReqLine = 0;
    int _aiReqCol = 0;

    std::mutex _suggestionMutex;
    std::vector<std::string> _suggestionLines;
    int _sugCursorLine = -1;
    int _sugCursorCol = -1;

public:
    Editor() {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
            SDL_Log("SDL_Init failed: %s", SDL_GetError());
        }

        if (!TTF_Init()) {
            SDL_Log("TTF_Init failed: %s", SDL_GetError());
        }

        _font = TTF_OpenFont("font.ttf", 24);
        if (!_font) {
            SDL_Log("TTF_OpenFont failed: %s", SDL_GetError());
        }

        _window = SDL_CreateWindow("NCode", _width, _height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED);
        if (!_window) {
            SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        }

        _renderer = SDL_CreateRenderer(_window, NULL);
        if (!_renderer) {
            SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        }

        _textEngine = TTF_CreateRendererTextEngine(_renderer);
        if (!_textEngine) {
            SDL_Log("TTF_CreateRendererTextEngine failed: %s", SDL_GetError());
        }

        if (_font && _textEngine) {
            TTF_Text* charText = TTF_CreateText(_textEngine, _font, "A", 1);
            if (charText) {
                int h;
                TTF_GetTextSize(charText, &_charWidth, &h);
                TTF_DestroyText(charText);
            }
        }

        llama_backend_init();

        _fileContent.push_back("");
        updateTextDisplay();

        SDL_StartTextInput(_window);
        updateDimensionsOnResize();
        _state = true;
        _isDirty = false;

        _aiThread = std::thread(&Editor::aiWorker, this);
    }

    void runEditor() {
        _change = true;

        while (_state) {
            if (_isFileReady) {
                std::string pathToLoad;
                {
                    std::lock_guard<std::mutex> lock(_fileMutex);
                    pathToLoad = _pendingFilePath;
                    _isFileReady = false;
                }
                processFileLoad(pathToLoad);
                _change = true;
            }

            if (_isSaveReady) {
                std::string pathToSave;
                {
                    std::lock_guard<std::mutex> lock(_fileMutex);
                    pathToSave = _pendingSavePath;
                    _isSaveReady = false;
                }
                _currentFilePath = pathToSave;
                if(_isDirty) {
                    saveFile(); 
                }
                _change = true;
            }

            if(_isFolderReady) {
                std::string folderToLoad;
                {
                    std::lock_guard<std::mutex> lock(_folderMutex);
                    folderToLoad = _pendingFolderPath;
                    _isFolderReady = false;
                }
                processFolderLoad(folderToLoad);
                _change = true;
            }

            if (_change) {
                SDL_SetRenderDrawColor(_renderer, 0, 0, 0, 255);
                SDL_RenderClear(_renderer);
                drawLayout();
                SDL_RenderPresent(_renderer);
                _change = false;
            }

            if (SDL_WaitEventTimeout(&_event, 100)) {
                processEvent(_event);
                while (SDL_PollEvent(&_event)) {
                    processEvent(_event);
                }
            }
        }
    }

    ~Editor() {
        _state = false;
        _aiThreadRunning = false;
        _aiCv.notify_all();
        if (_aiThread.joinable()) {
            _aiThread.join();
        }

        if (_rootFolder) {
            delete _rootFolder;
        }
        clearSidebarItems();
        clearRenderLines();
        if (_textEngine) TTF_DestroyRendererTextEngine(_textEngine);
        if (_font) TTF_CloseFont(_font);
        if (_renderer) SDL_DestroyRenderer(_renderer);
        if (_window) SDL_DestroyWindow(_window);
        TTF_Quit();
        SDL_Quit();
        llama_backend_free();
    }

private:
    void clearRenderLines() {
        for (auto& rl : _renderLines) {
            if (rl.numText) TTF_DestroyText(rl.numText);
            if (rl.contentText) TTF_DestroyText(rl.contentText);
        }
        _renderLines.clear();
    }

    void clearSidebarItems() {
        for (auto& item : _sidebarItems) {
            if (item.textRender) TTF_DestroyText(item.textRender);
        }
        _sidebarItems.clear();
    }

    void processEvent(const SDL_Event& event) {
        if (event.type == SDL_EVENT_QUIT) {
            _state = false;
        }
        else if (event.type == SDL_EVENT_TEXT_INPUT) {
            handleTextInput(event.text.text);
        }
        else if (event.type == SDL_EVENT_KEY_DOWN) {
            handleKeyDown(event.key);
        }
        else if (event.type == SDL_EVENT_WINDOW_RESIZED) {
            updateDimensionsOnResize();
            _change = true;
        }
        else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
            float mouseX, mouseY;
            SDL_GetMouseState(&mouseX, &mouseY);
            SDL_FPoint p = {mouseX, mouseY};
            
            if (SDL_PointInRectFloat(&p, &_sideBar)) {
                _sideScrollOffsetY -= event.wheel.y * 40.0f;
                _sideScrollOffsetX -= event.wheel.x * 40.0f;
                clampSideScrollbars();
            } else {
                _scrollOffsetY -= event.wheel.y * 40.0f;
                _scrollOffsetX -= event.wheel.x * 40.0f;
                clampAndUpdateScrollbars();
            }
            _change = true;
        }
        else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            if (event.button.button == SDL_BUTTON_LEFT) {
                handleMouseClick(event.button.x, event.button.y);
            }
        }
        else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            if (event.button.button == SDL_BUTTON_LEFT) {
                _isDraggingV = false;
                _isDraggingH = false;
                _isDraggingSideV = false;
                _isDraggingSideH = false;
                _isSelectingText = false;
                _change = true;
            }
        }
        else if (event.type == SDL_EVENT_MOUSE_MOTION) {
            handleMouseMotion(event.motion.x, event.motion.y);
        }
    }

    void requestAiSuggestion() {
        {
            std::lock_guard<std::mutex> lock(_suggestionMutex);
            _suggestionLines.clear();
            _change = true;
        }
        
        if (!_aiEnabled || _fileContent.empty()) return;

        std::string prefix;
        for (int i = 0; i < _cursorLine; ++i) {
            prefix += _fileContent[i] + "\n";
        }
        prefix += _fileContent[_cursorLine].substr(0, _cursorCol);

        std::string suffix = _fileContent[_cursorLine].substr(_cursorCol) + "\n";
        for (int i = _cursorLine + 1; i < (int)_fileContent.size(); ++i) {
            suffix += _fileContent[i] + "\n";
        }

        {
            std::lock_guard<std::mutex> lock(_aiWorkerMutex);
            _aiPrefix = prefix;
            _aiSuffix = suffix;
            _aiReqLine = _cursorLine;
            _aiReqCol = _cursorCol;
            _cancelGeneration = true;
            _pendingAiRequest = true;
        }
        _aiCv.notify_one();
    }

    void acceptAiSuggestion() {
        std::unique_lock<std::mutex> lock(_suggestionMutex);
        if (_suggestionLines.empty() || _sugCursorLine != _cursorLine || _sugCursorCol != _cursorCol) return;

        std::string currentLine = _fileContent[_cursorLine];
        std::string leftPart = currentLine.substr(0, _cursorCol);
        std::string rightPart = currentLine.substr(_cursorCol);

        if (_suggestionLines.size() == 1) {
            _fileContent[_cursorLine].insert(_cursorCol, _suggestionLines[0]);
            _cursorCol += _suggestionLines[0].length();
            updateSingleRenderLine(_cursorLine);
        } else {
            _fileContent[_cursorLine] = leftPart + _suggestionLines[0];
            std::vector<std::string> newLines;
            for(size_t i = 1; i < _suggestionLines.size(); ++i) {
                newLines.push_back(_suggestionLines[i]);
            }
            _cursorCol = newLines.back().length(); 
            newLines.back() += rightPart; 
            
            _fileContent.insert(_fileContent.begin() + _cursorLine + 1, newLines.begin(), newLines.end());
            _cursorLine += _suggestionLines.size() - 1;
            
            updateTextDisplay();
        }

        _suggestionLines.clear();
        forceCursorVisible();
        _change = true;
        _isDirty = true;

        lock.unlock(); 
        
        requestAiSuggestion();
    }

    void aiWorker() {
        llama_model* model = nullptr;
        
        while (_aiThreadRunning) {
            std::string prefix, suffix;
            int reqLine = 0, reqCol = 0;
            
            {
                std::unique_lock<std::mutex> lock(_aiWorkerMutex);
                _aiCv.wait(lock, [this] { return _pendingAiRequest || !_aiThreadRunning; });
                if (!_aiThreadRunning) break;

                _pendingAiRequest = false;
                _cancelGeneration = false;
                prefix = _aiPrefix;
                suffix = _aiSuffix;
                reqLine = _aiReqLine;
                reqCol = _aiReqCol;
            }

            if (!model) {
                llama_model_params mparams = llama_model_default_params();
                mparams.n_gpu_layers = 999; 
                mparams.main_gpu = 0; 
                mparams.split_mode = LLAMA_SPLIT_MODE_NONE;
                
                model = llama_model_load_from_file("qwen.gguf", mparams);
                if (!model) {
                    SDL_Log("Failed to load Qwen FIM model (qwen.gguf). Disabling AI.");
                    _aiEnabled = false;
                    continue;
                }
            }

            llama_context_params cparams = llama_context_default_params();
            
            cparams.n_ctx = 8192; 
            cparams.n_batch = 2048;
            
            llama_context* ctx = llama_init_from_model(model, cparams);
            
            if (!ctx) {
                SDL_Log("Failed to initialize llama_context. Check your VRAM or reduce n_ctx.");
                continue;
            }

            llama_sampler* sampler = llama_sampler_chain_init(llama_sampler_chain_default_params());
            llama_sampler_chain_add(sampler, llama_sampler_init_greedy());

            std::string prompt = "<|fim_prefix|>" + prefix + "<|fim_suffix|>" + suffix + "<|fim_middle|>";

            const llama_vocab* vocab = llama_model_get_vocab(model);

            std::vector<llama_token> tokens_list(prompt.size() + 16);
            int n_tokens = llama_tokenize(vocab, prompt.c_str(), prompt.size(), tokens_list.data(), tokens_list.size(), true, true);
            if (n_tokens < 0) {
                tokens_list.resize(-n_tokens);
                n_tokens = llama_tokenize(vocab, prompt.c_str(), prompt.size(), tokens_list.data(), tokens_list.size(), true, true);
            }

            if (n_tokens > cparams.n_ctx - 1024) {
                SDL_Log("File is too large for the 8192 token context window. Suggestion skipped.");
                llama_sampler_free(sampler);
                llama_free(ctx);
                continue;
            }

            llama_batch batch = llama_batch_init(cparams.n_batch, 0, 1);
            bool decode_failed = false;
            
            for (int i = 0; i < n_tokens; i += cparams.n_batch) {
                int eval_count = std::min((int)cparams.n_batch, n_tokens - i);
                batch.n_tokens = eval_count;
                
                for (int j = 0; j < eval_count; j++) {
                    batch.token[j] = tokens_list[i + j];
                    batch.pos[j] = i + j;
                    batch.n_seq_id[j] = 1;
                    batch.seq_id[j][0] = 0;
                    batch.logits[j] = false;
                }
                
                if (i + eval_count == n_tokens) {
                    batch.logits[eval_count - 1] = true;
                }
                
                if (llama_decode(ctx, batch) != 0) {
                    SDL_Log("llama_decode() failed during prompt evaluation");
                    decode_failed = true;
                    break;
                }
            }

            if (decode_failed) {
                llama_batch_free(batch);
                llama_sampler_free(sampler);
                llama_free(ctx);
                continue;
            }

            std::vector<std::string> cur_suggestion_lines = {""};
            int n_cur = n_tokens;
            int max_gen = 2048; 

            for (int i = 0; i < max_gen; ++i) {
                if (_cancelGeneration) break;

                llama_token new_token_id = llama_sampler_sample(sampler, ctx, -1);
                llama_sampler_accept(sampler, new_token_id);

                if (llama_vocab_is_eog(vocab, new_token_id)) break;

                char buf[128];
                int n = llama_token_to_piece(vocab, new_token_id, buf, sizeof(buf), 0, true);
                if (n > 0) {
                    std::string piece(buf, n);
                    for (char c : piece) {
                        if (c == '\n') cur_suggestion_lines.push_back("");
                        else if (c == '\r') continue;
                        else cur_suggestion_lines.back() += c;
                    }

                    {
                        std::lock_guard<std::mutex> lock(_suggestionMutex);
                        _suggestionLines = cur_suggestion_lines;
                        _sugCursorLine = reqLine;
                        _sugCursorCol = reqCol;
                        _change = true; 
                    }
                }

                batch.token[0] = new_token_id;
                batch.pos[0] = n_cur++;
                batch.n_seq_id[0] = 1;
                batch.seq_id[0][0] = 0;
                batch.logits[0] = true;
                batch.n_tokens = 1;

                if (llama_decode(ctx, batch) != 0) break;
            }
            
            llama_batch_free(batch);
            llama_sampler_free(sampler);
            llama_free(ctx);
        }

        if (model) llama_model_free(model);
    }

    void handleTextInput(const char* text) {
        clearSelection();
        _fileContent[_cursorLine].insert(_cursorCol, text);
        _cursorCol += strlen(text);
        
        updateSingleRenderLine(_cursorLine);
        forceCursorVisible();
        _change = true;
        _isDirty = true;

        requestAiSuggestion();
    }

    void handleKeyDown(const SDL_KeyboardEvent& key) {
        bool ctrlPressed = (key.mod & SDL_KMOD_CTRL);

        if (key.key == SDLK_ESCAPE) {
            _state = false;
        }
        else if (ctrlPressed && key.key == SDLK_I) {
            _aiEnabled = !_aiEnabled;
            if (_aiEnabled) {
                requestAiSuggestion();
            } else {
                std::lock_guard<std::mutex> lock(_suggestionMutex);
                _suggestionLines.clear();
                _change = true;
            }
        }
        else if (ctrlPressed && key.key == SDLK_TAB) {
            acceptAiSuggestion();
        }
        else if (ctrlPressed && key.key == SDLK_C) {
            copySelectionToClipboard();
        }
        else if (ctrlPressed && key.key == SDLK_V) {
            handlePaste();
        }
        else if (ctrlPressed && key.key == SDLK_S) {
            saveFile();
        }
        else if (ctrlPressed && key.key == SDLK_A){
            selectAllText();
        }
        else if (key.key == SDLK_BACKSPACE) {
            handleBackspace();
        }
        else if (key.key == SDLK_RETURN || key.key == SDLK_KP_ENTER) {
            handleEnter();
        }
        else if (key.key == SDLK_TAB) {
            handleTab();
        }
        else if (key.key == SDLK_LEFT) {
            clearSelection();
            if (_cursorCol > 0) {
                _cursorCol--;
            } else if (_cursorLine > 0) {
                _cursorLine--;
                _cursorCol = _fileContent[_cursorLine].length();
            }
            forceCursorVisible();
            requestAiSuggestion();
        }
        else if (key.key == SDLK_RIGHT) {
            clearSelection();
            if (_cursorCol < _fileContent[_cursorLine].length()) {
                _cursorCol++;
            } else if (_cursorLine < _fileContent.size() - 1) {
                _cursorLine++;
                _cursorCol = 0;
            }
            forceCursorVisible();
            requestAiSuggestion();
        }
        else if (key.key == SDLK_UP) {
            clearSelection();
            if (_cursorLine > 0) {
                _cursorLine--;
                _cursorCol = std::min(_cursorCol, (int)_fileContent[_cursorLine].length());
            }
            forceCursorVisible();
            requestAiSuggestion();
        }
        else if (key.key == SDLK_DOWN) {
            clearSelection();
            if (_cursorLine < _fileContent.size() - 1) {
                _cursorLine++;
                _cursorCol = std::min(_cursorCol, (int)_fileContent[_cursorLine].length());
            }
            forceCursorVisible();
            requestAiSuggestion();
        }
    }

    void handleTab() {
        clearSelection();
        std::string spaces = "    "; 
        _fileContent[_cursorLine].insert(_cursorCol, spaces);
        _cursorCol += spaces.length();
        
        updateSingleRenderLine(_cursorLine);
        forceCursorVisible();
        _change = true;
        _isDirty = true;
        
        requestAiSuggestion();
    }

    void handleBackspace() {
        if (_fileContent.empty()) return;
        clearSelection();

        if (_cursorCol > 0) {
            _fileContent[_cursorLine].erase(_cursorCol - 1, 1);
            _cursorCol--;
            updateSingleRenderLine(_cursorLine);
        } else if (_cursorLine > 0) {
            int prevLen = _fileContent[_cursorLine - 1].length();
            _fileContent[_cursorLine - 1] += _fileContent[_cursorLine];
            
            _fileContent.erase(_fileContent.begin() + _cursorLine);
            _cursorLine--;
            _cursorCol = prevLen;
            
            updateTextDisplay();
        }
        forceCursorVisible();
        _change = true;
        _isDirty = true;
        
        requestAiSuggestion();
    }

    void handleEnter() {
        clearSelection();
        std::string currentLine = _fileContent[_cursorLine];
        std::string leftPart = currentLine.substr(0, _cursorCol);
        std::string rightPart = currentLine.substr(_cursorCol);
        
        _fileContent[_cursorLine] = leftPart;
        
        _cursorLine++;
        _cursorCol = 0;
        _fileContent.insert(_fileContent.begin() + _cursorLine, rightPart);
        
        updateTextDisplay();
        forceCursorVisible();
        _change = true;
        _isDirty = true;
        
        requestAiSuggestion();
    }

    void handlePaste() {
        if (!SDL_HasClipboardText()) return;
        char* text = SDL_GetClipboardText();
        if (!text) return;
        
        std::string pasteText(text);
        SDL_free(text);
        
        clearSelection();

        std::stringstream ss(pasteText);
        std::string line;

        std::vector<std::string> pastedLines;
        while(std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back(); 
            pastedLines.push_back(line);
        }

        if (pastedLines.empty()) return;

        std::string currentLine = _fileContent[_cursorLine];
        std::string leftPart = currentLine.substr(0, _cursorCol);
        std::string rightPart = currentLine.substr(_cursorCol);

        if (pastedLines.size() == 1) {
            _fileContent[_cursorLine].insert(_cursorCol, pastedLines[0]);
            _cursorCol += pastedLines[0].length();
            updateSingleRenderLine(_cursorLine);
        } else {
            _fileContent[_cursorLine] = leftPart + pastedLines[0];
            _fileContent.insert(_fileContent.begin() + _cursorLine + 1, 
                                pastedLines.begin() + 1, 
                                pastedLines.end());
            _cursorLine += pastedLines.size() - 1;
            _cursorCol = pastedLines.back().length();
            _fileContent[_cursorLine] += rightPart;
            updateTextDisplay();
        }

        forceCursorVisible();
        _change = true;
        _isDirty = true;
        
        requestAiSuggestion();
    }

    void saveFile() {
        if (!_isDirty) return;

        if (_currentFilePath.empty()) {
            SDL_ShowSaveFileDialog(Editor::saveFileCallback, this, _window, nullptr, 0, nullptr);
            return;
        }

        std::ofstream file(_currentFilePath);
        if (file.is_open()) {
            for (const auto& line : _fileContent) {
                file << line << "\n";
            }
            file.close();
            SDL_Log("Successfully saved to %s", _currentFilePath.c_str());
            _isDirty = false; 
        }
    }

    void forceCursorVisible() {
        int lineHeight = TTF_GetFontHeight(_font);
        if (lineHeight <= 0) lineHeight = 24;

        float cursorY = _cursorLine * lineHeight;
        float textDisplayH = _textArea.h - SCROLLBAR_SIZE;

        if (cursorY < _scrollOffsetY) {
            _scrollOffsetY = cursorY; 
        } else if (cursorY + lineHeight > _scrollOffsetY + textDisplayH) {
            _scrollOffsetY = cursorY + lineHeight - textDisplayH; 
        }

        float cursorX = _renderLines[_cursorLine].numW + (_cursorCol * _charWidth);
        float textDisplayW = _textArea.w - SCROLLBAR_SIZE;

        if (cursorX < _scrollOffsetX) {
            _scrollOffsetX = std::max(0.0f, cursorX - 20.0f); 
        } else if (cursorX + _charWidth > _scrollOffsetX + textDisplayW) {
            _scrollOffsetX = cursorX + _charWidth - textDisplayW + 20.0f; 
        }

        clampAndUpdateScrollbars();
    }

    void clearSelection() {
        _selStartLine = _selEndLine = -1;
        {
            std::lock_guard<std::mutex> lock(_suggestionMutex);
            _suggestionLines.clear();
        }
    }

    void updateSingleRenderLine(int lineIdx) {
        if (lineIdx < 0 || lineIdx >= _fileContent.size()) return;
        
        RenderLine& rl = _renderLines[lineIdx];
        if (rl.contentText) TTF_DestroyText(rl.contentText);
        
        std::string contentDisplay = _fileContent[lineIdx];
        if (contentDisplay.empty()) contentDisplay = " ";

        rl.contentText = TTF_CreateText(_textEngine, _font, contentDisplay.c_str(), contentDisplay.length());
        if (rl.contentText) {
            TTF_GetTextSize(rl.contentText, &rl.contentW, &rl.contentH);
            TTF_SetTextColor(rl.contentText, 22, 223, 45, 255);
            
            int totalLineWidth = rl.numW + rl.contentW;
            if (totalLineWidth > _textWidth) {
                _textWidth = totalLineWidth;
                clampAndUpdateScrollbars();
            }
        }
    }

    void handleMouseClick(float mouseX, float mouseY) {
        SDL_FPoint clickPoint = { mouseX, mouseY };
        float displayW = _textArea.w - SCROLLBAR_SIZE;
        float displayH = _textArea.h - SCROLLBAR_SIZE;

        if (_textHeight > displayH - 10.0f && SDL_PointInRectFloat(&clickPoint, &_vThumb)) {
            _isDraggingV = true;
            _dragOffsetY = mouseY - _vThumb.y;
            return;
        }
        if (_textWidth > displayW - 10.0f && SDL_PointInRectFloat(&clickPoint, &_hThumb)) {
            _isDraggingH = true;
            _dragOffsetX = mouseX - _hThumb.x;
            return;
        }

        float sideDisplayW = _sideBar.w - SCROLLBAR_SIZE;
        float sideDisplayH = _sideBar.h - SCROLLBAR_SIZE;

        if (_sideTextHeight > sideDisplayH - 10.0f && SDL_PointInRectFloat(&clickPoint, &_sideVThumb)) {
            _isDraggingSideV = true;
            _dragSideOffsetY = mouseY - _sideVThumb.y;
            return;
        }
        if (_sideTextWidth > sideDisplayW - 10.0f && SDL_PointInRectFloat(&clickPoint, &_sideHThumb)) {
            _isDraggingSideH = true;
            _dragSideOffsetX = mouseX - _sideHThumb.x;
            return;
        }

        for (const auto& hb : _hitBoxes) {
            if (SDL_PointInRectFloat(&clickPoint, &hb.box)) {
                if (hb.name == "FileText") {
                    SDL_ShowOpenFileDialog(Editor::loadFileCallback, this, _window, nullptr, 0, nullptr, false);
                    return; 
                }
                else if(hb.name == "FolderText") {
                    SDL_ShowOpenFolderDialog(Editor::loadFolderCallback, this, _window, nullptr, false);
                    return; 
                }
                else if(hb.name == "NewText") {
                    if (_isDirty) saveFile();
                    _fileContent.clear();
                    _fileContent.push_back("");
                    _cursorLine = 0;
                    _cursorCol = 0;
                    clearSelection();
                    updateTextDisplay();
                    _currentFilePath = "";
                    _change = true;
                    _isDirty = false;
                    return; 
                }
            }
        }

        if (SDL_PointInRectFloat(&clickPoint, &_sideBar)) {
            int lineHeight = TTF_GetFontHeight(_font);
            if (lineHeight <= 0) lineHeight = 24;

            float relativeY = mouseY - (_sideBar.y + 5.0f) + _sideScrollOffsetY;
            int clickedIndex = (int)(relativeY / lineHeight);

            if (clickedIndex >= 0 && clickedIndex < _sidebarItems.size()) {
                SideBarItem& clickedItem = _sidebarItems[clickedIndex];
                
                if (clickedItem.isFolder && clickedItem.folderPtr) {
                    clickedItem.folderPtr->isExpanded = !clickedItem.folderPtr->isExpanded;
                    updateFolderView();
                } else {
                    if (_isDirty) saveFile();
                    processFileLoad(clickedItem.path);
                }
                _change = true;
            }
            return;
        }

        if (SDL_PointInRectFloat(&clickPoint, &_textArea)) {
            _isSelectingText = true;
            getLineAndColFromMouse(mouseX, mouseY, _selStartLine, _selStartCol);
            
            _cursorLine = _selStartLine;
            _cursorCol = _selStartCol;
            forceCursorVisible();

            _selEndLine = _selStartLine;
            _selEndCol = _selStartCol;
            _change = true;
            
            requestAiSuggestion();
        } else {
            clearSelection();
            _change = true;
        }
    }

    void handleMouseMotion(float mouseX, float mouseY) {
        if (_isDraggingV || _isDraggingH) {
            float textDisplayW = _textArea.w - SCROLLBAR_SIZE;
            float textDisplayH = _textArea.h - SCROLLBAR_SIZE;
            
            float maxScrollY = std::max(0.0f, (float)_textHeight - textDisplayH + 10.0f);
            float maxScrollX = std::max(0.0f, (float)_textWidth - textDisplayW + 10.0f);

            if (_isDraggingV && maxScrollY > 0) {
                float maxThumbY = textDisplayH - _vThumb.h;
                float newThumbY = mouseY - _dragOffsetY - _textArea.y;
                newThumbY = std::clamp(newThumbY, 0.0f, maxThumbY);
                _scrollOffsetY = (newThumbY / maxThumbY) * maxScrollY;
            }

            if (_isDraggingH && maxScrollX > 0) {
                float maxThumbX = textDisplayW - _hThumb.w;
                float newThumbX = mouseX - _dragOffsetX - _textArea.x;
                newThumbX = std::clamp(newThumbX, 0.0f, maxThumbX);
                _scrollOffsetX = (newThumbX / maxThumbX) * maxScrollX;
            }

            clampAndUpdateScrollbars();
            _change = true;
        }

        if (_isDraggingSideV || _isDraggingSideH) {
            float displayW = _sideBar.w - SCROLLBAR_SIZE;
            float displayH = _sideBar.h - SCROLLBAR_SIZE;
            
            float maxScrollY = std::max(0.0f, (float)_sideTextHeight - displayH + 10.0f);
            float maxScrollX = std::max(0.0f, (float)_sideTextWidth - displayW + 10.0f);

            if (_isDraggingSideV && maxScrollY > 0) {
                float maxThumbY = displayH - _sideVThumb.h;
                float newThumbY = mouseY - _dragSideOffsetY - _sideBar.y;
                newThumbY = std::clamp(newThumbY, 0.0f, maxThumbY);
                _sideScrollOffsetY = (newThumbY / maxThumbY) * maxScrollY;
            }

            if (_isDraggingSideH && maxScrollX > 0) {
                float maxThumbX = displayW - _sideHThumb.w;
                float newThumbX = mouseX - _dragSideOffsetX - _sideBar.x;
                newThumbX = std::clamp(newThumbX, 0.0f, maxThumbX);
                _sideScrollOffsetX = (newThumbX / maxThumbX) * maxScrollX;
            }

            clampSideScrollbars();
            _change = true;
        }

        if (_isSelectingText) {
            getLineAndColFromMouse(mouseX, mouseY, _selEndLine, _selEndCol);
            _cursorLine = _selEndLine;
            _cursorCol = _selEndCol;
            forceCursorVisible(); 
            _change = true;
        }
    }

    void clampAndUpdateScrollbars() {
        float textDisplayW = _textArea.w - SCROLLBAR_SIZE;
        float textDisplayH = _textArea.h - SCROLLBAR_SIZE;

        float maxScrollX = std::max(0.0f, (float)_textWidth - textDisplayW + 10.0f);
        float maxScrollY = std::max(0.0f, (float)_textHeight - textDisplayH + 10.0f);

        _scrollOffsetX = std::clamp(_scrollOffsetX, 0.0f, maxScrollX);
        _scrollOffsetY = std::clamp(_scrollOffsetY, 0.0f, maxScrollY);

        if (_textHeight > textDisplayH - 10.0f && _textHeight > 0) {
            _vThumb.h = std::max(20.0f, textDisplayH * (textDisplayH / _textHeight));
            float maxThumbY = textDisplayH - _vThumb.h;
            _vThumb.y = _textArea.y + (_scrollOffsetY / maxScrollY) * maxThumbY;
        } else {
            _vThumb.h = textDisplayH;
            _vThumb.y = _textArea.y;
        }
        _vThumb.x = _textArea.x + textDisplayW;
        _vThumb.w = SCROLLBAR_SIZE;

        if (_textWidth > textDisplayW - 10.0f && _textWidth > 0) {
            _hThumb.w = std::max(20.0f, textDisplayW * (textDisplayW / _textWidth));
            float maxThumbX = textDisplayW - _hThumb.w;
            _hThumb.x = _textArea.x + (_scrollOffsetX / maxScrollX) * maxThumbX;
        } else {
            _hThumb.w = textDisplayW;
            _hThumb.x = _textArea.x;
        }
        _hThumb.y = _textArea.y + textDisplayH;
        _hThumb.h = SCROLLBAR_SIZE;
    }

    void clampSideScrollbars() {
        float displayW = _sideBar.w - SCROLLBAR_SIZE;
        float displayH = _sideBar.h - SCROLLBAR_SIZE;

        float maxScrollX = std::max(0.0f, (float)_sideTextWidth - displayW + 10.0f);
        float maxScrollY = std::max(0.0f, (float)_sideTextHeight - displayH + 10.0f);

        _sideScrollOffsetX = std::clamp(_sideScrollOffsetX, 0.0f, maxScrollX);
        _sideScrollOffsetY = std::clamp(_sideScrollOffsetY, 0.0f, maxScrollY);

        if (_sideTextHeight > displayH - 10.0f && _sideTextHeight > 0) {
            _sideVThumb.h = std::max(20.0f, displayH * (displayH / _sideTextHeight));
            float maxThumbY = displayH - _sideVThumb.h;
            _sideVThumb.y = _sideBar.y + (_sideScrollOffsetY / maxScrollY) * maxThumbY;
        } else {
            _sideVThumb.h = displayH;
            _sideVThumb.y = _sideBar.y;
        }
        _sideVThumb.x = _sideBar.x + displayW;
        _sideVThumb.w = SCROLLBAR_SIZE;

        if (_sideTextWidth > displayW - 10.0f && _sideTextWidth > 0) {
            _sideHThumb.w = std::max(20.0f, displayW * (displayW / _sideTextWidth));
            float maxThumbX = displayW - _sideHThumb.w;
            _sideHThumb.x = _sideBar.x + (_sideScrollOffsetX / maxScrollX) * maxThumbX;
        } else {
            _sideHThumb.w = displayW;
            _sideHThumb.x = _sideBar.x;
        }
        _sideHThumb.y = _sideBar.y + displayH;
        _sideHThumb.h = SCROLLBAR_SIZE;
    }

    void updateDimensionsOnResize() {
        SDL_Rect displayBounds;
        SDL_GetWindowSize(_window, &displayBounds.w, &displayBounds.h);
        
        _width = displayBounds.w;
        _height = displayBounds.h;

        _titleBar = { 0.0f, 0.0f, (float)_width, 40.0f };
        _sideBar = { 0.0f, 40.0f, (float)_width * 0.2f, (float)_height - 40.0f };
        _textArea = { (float)_width * 0.2f, 40.0f, (float)_width * 0.8f, (float)_height - 40.0f };

        updateHitboxes();
        clampAndUpdateScrollbars();
        clampSideScrollbars();
    }

    void loadFolderContentsRecursive(Folder* folder) {
        if (!folder) return;
        folder->childFolders.clear();
        folder->files.clear();

        SDL_EnumerateDirectory(folder->path.c_str(), DirEnumCallback, folder);

        auto sortFolders = [](Folder* a, Folder* b) {
            std::string nameA = a->name;
            std::string nameB = b->name;
            for(auto& c : nameA) c = std::tolower(static_cast<unsigned char>(c));
            for(auto& c : nameB) c = std::tolower(static_cast<unsigned char>(c));
            return nameA < nameB;
        };

        auto sortFiles = [](const std::string& a, const std::string& b) {
            std::string nameA = a;
            std::string nameB = b;
            for(auto& c : nameA) c = std::tolower(static_cast<unsigned char>(c));
            for(auto& c : nameB) c = std::tolower(static_cast<unsigned char>(c));
            return nameA < nameB;
        };

        std::sort(folder->childFolders.begin(), folder->childFolders.end(), sortFolders);
        std::sort(folder->files.begin(), folder->files.end(), sortFiles);

        for (Folder* child : folder->childFolders) {
            loadFolderContentsRecursive(child);
        }
    }

    void buildSidebarView(Folder* folder, int depth) {
        if (!folder) return;
        
        SideBarItem item;
        item.isFolder = true;
        item.folderPtr = folder;
        item.name = folder->name;
        item.path = folder->path;
        item.depth = depth;
        item.textRender = nullptr;
        _sidebarItems.push_back(item);

        if (folder->isExpanded) {
            for (Folder* child : folder->childFolders) {
                buildSidebarView(child, depth + 1);
            }
            for (const std::string& file : folder->files) {
                SideBarItem fItem;
                fItem.isFolder = false;
                fItem.folderPtr = nullptr;
                fItem.name = file;
                
                std::string dir = folder->path;
                if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += "/";
                
                fItem.path = dir + file;
                fItem.depth = depth + 1;
                fItem.textRender = nullptr;
                _sidebarItems.push_back(fItem);
            }
        }
    }

    void updateFolderView() {
        clearSidebarItems();
        
        if (_rootFolder) {
            buildSidebarView(_rootFolder, 0);
        }
        
        int lineHeight = TTF_GetFontHeight(_font);
        if (lineHeight <= 0) lineHeight = 24;
        
        _sideTextHeight = _sidebarItems.size() * lineHeight;
        _sideTextWidth = 0;
        
        for (auto& item : _sidebarItems) {
            std::string prefix = item.isFolder ? (item.folderPtr->isExpanded ? "v " : "> ") : "  ";
            std::string displayText = prefix + item.name;
            
            item.textRender = TTF_CreateText(_textEngine, _font, displayText.c_str(), displayText.length());
            if (item.textRender) {
                TTF_GetTextSize(item.textRender, &item.w, &item.h);
                if (item.isFolder) {
                    TTF_SetTextColor(item.textRender, 100, 200, 255, 255);  
                } else {
                    TTF_SetTextColor(item.textRender, 200, 200, 200, 255); 
                }
            }

            int itemW = item.w + (item.depth * 15) + 20;
            if (itemW > _sideTextWidth) {
                _sideTextWidth = itemW;
            }
        }
        clampSideScrollbars();
    }

    void processFolderLoad(const std::string& folderPath) {
        _currentFolderPath = folderPath;
        if (_rootFolder) {
            delete _rootFolder;
            _rootFolder = nullptr;
        }
        
        _rootFolder = new Folder();
        _rootFolder->path = folderPath;
        
        std::string cleanPath = folderPath;
        while (!cleanPath.empty() && (cleanPath.back() == '/' || cleanPath.back() == '\\')) {
            cleanPath.pop_back();
        }
        
        size_t lastSlash = cleanPath.find_last_of("/\\");
        if (lastSlash != std::string::npos) {
            _rootFolder->name = cleanPath.substr(lastSlash + 1);
        } else {
            _rootFolder->name = cleanPath.empty() ? folderPath : cleanPath; 
        }
        
        _rootFolder->isExpanded = true;
        
        loadFolderContentsRecursive(_rootFolder);
        updateFolderView();
        
        _fileContent.clear();
        _fileContent.push_back("");
        updateTextDisplay();
        
        _cursorLine = 0;
        _cursorCol = 0;
        clearSelection();
        
        _sideScrollOffsetX = 0.0f;
        _sideScrollOffsetY = 0.0f;
        _change = true;
        _isDirty = false;
    }

    void updateHitboxes() {
        _hitBoxes.clear();
        TTF_Text* fileText = TTF_CreateText(_textEngine, _font, "File", 4);
        int textW = 0, textH = 0;
        
        if (fileText) {
            TTF_GetTextSize(fileText, &textW, &textH);
            TTF_DestroyText(fileText);
        }

        hitBox fileButton;
        fileButton.name = "FileText";
        fileButton.box = { 0.0f, 0.0f, (float)textW, (float)textH };
        _hitBoxes.push_back(fileButton);

        fileText = TTF_CreateText(_textEngine, _font, "Folder", 6);
        if (fileText) {
            TTF_GetTextSize(fileText, &textW, &textH);
            TTF_DestroyText(fileText);
        }

        hitBox folderButton;
        folderButton.name = "FolderText";
        folderButton.box = { (float)(fileButton.box.x + fileButton.box.w + 10), 0.0f, (float)textW, (float)textH };
        _hitBoxes.push_back(folderButton);

        fileText = TTF_CreateText(_textEngine, _font, "New", 3);
        if (fileText) {
            TTF_GetTextSize(fileText, &textW, &textH);
            TTF_DestroyText(fileText);
        }

        hitBox newButton;
        newButton.name = "NewText";
        newButton.box = { (float)(folderButton.box.x + folderButton.box.w + 10), 0.0f, (float)textW, (float)textH };
        _hitBoxes.push_back(newButton);
    }

    void drawLayout() {
        drawTitleBar();
        drawSideBar();
        drawTextArea();
    }

    void drawTitleBar() {
        SDL_SetRenderDrawColor(_renderer, 40, 40, 40, 255);
        SDL_RenderFillRect(_renderer, &_titleBar);

        TTF_Text* fileOptions = TTF_CreateText(_textEngine, _font, "File", 4);
        if (fileOptions) {
            SDL_SetRenderDrawColor(_renderer, 255, 255, 255, 255);
            TTF_DrawRendererText(fileOptions, _hitBoxes[0].box.x, 0);
            TTF_DestroyText(fileOptions);
        }
        fileOptions = TTF_CreateText(_textEngine, _font, "Folder", 6);
        if (fileOptions) {
            SDL_SetRenderDrawColor(_renderer, 255, 255, 255, 255);
            TTF_DrawRendererText(fileOptions, _hitBoxes[1].box.x, 0);
            TTF_DestroyText(fileOptions);
        }
        fileOptions = TTF_CreateText(_textEngine, _font, "New", 3);
        if (fileOptions) {
            SDL_SetRenderDrawColor(_renderer, 255, 255, 255, 255);
            TTF_DrawRendererText(fileOptions, _hitBoxes[2].box.x, 0);
            TTF_DestroyText(fileOptions);
        }

        std::string aiStatus = _aiEnabled ? "AI: ON" : "AI: OFF";
        TTF_Text* aiOptions = TTF_CreateText(_textEngine, _font, aiStatus.c_str(), aiStatus.length());
        if (aiOptions) {
            TTF_SetTextColor(aiOptions, _aiEnabled ? 0 : 255, _aiEnabled ? 255 : 0, 0, 255);
            TTF_DrawRendererText(aiOptions, _hitBoxes[2].box.x + _hitBoxes[2].box.w + 30, 0);
            TTF_DestroyText(aiOptions);
        }
    }

    void drawSideBar() {
        SDL_SetRenderDrawColor(_renderer, 30, 30, 30, 255);
        SDL_RenderFillRect(_renderer, &_sideBar);

        float displayW = _sideBar.w - SCROLLBAR_SIZE;
        float displayH = _sideBar.h - SCROLLBAR_SIZE;

        if (!_sidebarItems.empty()) {
            SDL_Rect clipRect = {
                (int)_sideBar.x,
                (int)_sideBar.y,
                (int)displayW,
                (int)displayH
            };
            SDL_SetRenderClipRect(_renderer, &clipRect);

            int lineHeight = TTF_GetFontHeight(_font);
            if (lineHeight <= 0) lineHeight = 24;

            int firstVisibleLine = std::max(0, (int)(_sideScrollOffsetY / lineHeight));
            int visibleLineCount = std::ceil(clipRect.h / (float)lineHeight) + 1;
            int lastVisibleLine = std::min((int)_sidebarItems.size(), firstVisibleLine + visibleLineCount);

            for (int i = firstVisibleLine; i < lastVisibleLine; ++i) {
                float yPos = _sideBar.y + 5.0f - _sideScrollOffsetY + (i * lineHeight);
                float xPos = _sideBar.x + 5.0f - _sideScrollOffsetX + (_sidebarItems[i].depth * 15.0f);

                if (_sidebarItems[i].textRender) {
                    TTF_DrawRendererText(_sidebarItems[i].textRender, xPos, yPos);
                }
            }
            SDL_SetRenderClipRect(_renderer, nullptr);
        }

        SDL_SetRenderDrawColor(_renderer, 40, 40, 40, 255);
        SDL_FRect vTrack = { _sideBar.x + displayW, _sideBar.y, SCROLLBAR_SIZE, displayH };
        SDL_FRect hTrack = { _sideBar.x, _sideBar.y + displayH, displayW, SCROLLBAR_SIZE };
        SDL_FRect corner = { _sideBar.x + displayW, _sideBar.y + displayH, SCROLLBAR_SIZE, SCROLLBAR_SIZE };
        
        SDL_RenderFillRect(_renderer, &vTrack);
        SDL_RenderFillRect(_renderer, &hTrack);
        SDL_RenderFillRect(_renderer, &corner);

        if (_sideTextWidth > displayW) {
            SDL_SetRenderDrawColor(_renderer, _isDraggingSideH ? 120 : 80, _isDraggingSideH ? 120 : 80, _isDraggingSideH ? 120 : 80, 255);
            SDL_RenderFillRect(_renderer, &_sideHThumb);
        }
        
        if (_sideTextHeight > displayH) {
            SDL_SetRenderDrawColor(_renderer, _isDraggingSideV ? 120 : 80, _isDraggingSideV ? 120 : 80, _isDraggingSideV ? 120 : 80, 255);
            SDL_RenderFillRect(_renderer, &_sideVThumb);
        }
    }

    void drawTextArea() {
        SDL_SetRenderDrawColor(_renderer, 20, 20, 20, 255);
        SDL_RenderFillRect(_renderer, &_textArea);

        SDL_Rect clipRect = {
            (int)_textArea.x,
            (int)_textArea.y,
            (int)(_textArea.w - SCROLLBAR_SIZE),
            (int)(_textArea.h - SCROLLBAR_SIZE)
        };
        SDL_SetRenderClipRect(_renderer, &clipRect);
        
        int lineHeight = TTF_GetFontHeight(_font);
        if (lineHeight <= 0) lineHeight = 24;

        std::vector<std::string> currentSuggestions;
        int sugLine = -1, sugCol = -1;
        {
            std::lock_guard<std::mutex> lock(_suggestionMutex);
            currentSuggestions = _suggestionLines;
            sugLine = _sugCursorLine;
            sugCol = _sugCursorCol;
        }

        int sL = _selStartLine, sC = _selStartCol;
        int eL = _selEndLine, eC = _selEndCol;
        bool hasSelection = (sL != -1 && eL != -1) && (sL != eL || sC != eC);
        if (hasSelection && (sL > eL || (sL == eL && sC > eC))) {
            std::swap(sL, eL);
            std::swap(sC, eC);
        }

        bool showSuggestion = _aiEnabled && !currentSuggestions.empty() && 
                              _cursorLine == sugLine && _cursorCol == sugCol && !hasSelection;
        int numSugLines = showSuggestion ? currentSuggestions.size() : 0;

        if (!_renderLines.empty()) {
            int startIdx = std::max(0, (int)(_scrollOffsetY / lineHeight) - (showSuggestion ? numSugLines : 0));
            int endIdx = std::min((int)_renderLines.size(), startIdx + (int)std::ceil(clipRect.h / (float)lineHeight) + (showSuggestion ? numSugLines : 0) + 1);

            for (int i = startIdx; i < endIdx; ++i) {
                int shift = (showSuggestion && i > _cursorLine) ? (numSugLines - 1) : 0;
                float yPos = _textArea.y + 5.0f - _scrollOffsetY + (i + shift) * lineHeight;

                if (yPos > _textArea.y + _textArea.h) continue;
                if (yPos + (showSuggestion && i == _cursorLine ? numSugLines * lineHeight : lineHeight) < _textArea.y) continue;

                if (hasSelection && i >= sL && i <= eL) {
                    int col1 = (i == sL) ? sC : 0;
                    int col2 = (i == eL) ? eC : _fileContent[i].length();

                    int w1 = col1 * _charWidth;
                    int w2 = col2 * _charWidth;

                    float hlXPos = _textArea.x + 5.0f - _scrollOffsetX + _renderLines[i].numW;
                    float hlWidth = (float)(w2 - w1);
                    
                    if (hlWidth == 0 && i < eL) hlWidth = 8.0f; 

                    SDL_FRect hlRect = { hlXPos + w1, yPos, hlWidth, (float)lineHeight };

                    SDL_SetRenderDrawColor(_renderer, 0, 120, 215, 255);
                    SDL_RenderFillRect(_renderer, &hlRect);
                }

                if (_renderLines[i].numText) {
                    float numXPos = _textArea.x + 5.0f - _scrollOffsetX;
                    TTF_DrawRendererText(_renderLines[i].numText, numXPos, yPos);
                }

                if (_renderLines[i].contentText) {
                    float textXPos = _textArea.x + 5.0f - _scrollOffsetX + _renderLines[i].numW;
                    TTF_DrawRendererText(_renderLines[i].contentText, textXPos, yPos);
                }
                
                if (i == _cursorLine && _cursorLine < _renderLines.size()) {
                    float cursorX = _textArea.x + 5.0f - _scrollOffsetX + _renderLines[i].numW + (_cursorCol * _charWidth);
                    SDL_FRect cursorRect = { cursorX, yPos, 2.0f, (float)lineHeight };
                    
                    SDL_SetRenderDrawColor(_renderer, 0, 236, 234, 255);
                    SDL_RenderFillRect(_renderer, &cursorRect);

                    if (showSuggestion) {
                        for (size_t j = 0; j < currentSuggestions.size(); ++j) {
                            if (currentSuggestions[j].empty()) continue;

                            float sugY = yPos + (j * lineHeight);
                            float sugX = _textArea.x + 5.0f - _scrollOffsetX + _renderLines[i].numW;
                            if (j == 0) {
                                sugX += (_cursorCol * _charWidth); 
                            }

                            TTF_Text* sText = TTF_CreateText(_textEngine, _font, currentSuggestions[j].c_str(), currentSuggestions[j].length());
                            if (sText) {
                                TTF_SetTextColor(sText, 150, 150, 150, 255);
                                TTF_DrawRendererText(sText, sugX, sugY);
                                TTF_DestroyText(sText);
                            }
                        }
                    }
                }
            }
        }
        SDL_SetRenderClipRect(_renderer, nullptr);

        SDL_SetRenderDrawColor(_renderer, 40, 40, 40, 255);
        SDL_FRect vTrack = { _textArea.x + _textArea.w - SCROLLBAR_SIZE, _textArea.y, SCROLLBAR_SIZE, _textArea.h - SCROLLBAR_SIZE };
        SDL_FRect hTrack = { _textArea.x, _textArea.y + _textArea.h - SCROLLBAR_SIZE, _textArea.w - SCROLLBAR_SIZE, SCROLLBAR_SIZE };
        SDL_FRect corner = { _textArea.x + _textArea.w - SCROLLBAR_SIZE, _textArea.y + _textArea.h - SCROLLBAR_SIZE, SCROLLBAR_SIZE, SCROLLBAR_SIZE };
        
        SDL_RenderFillRect(_renderer, &vTrack);
        SDL_RenderFillRect(_renderer, &hTrack);
        SDL_RenderFillRect(_renderer, &corner);

        if (_textWidth > _textArea.w - SCROLLBAR_SIZE) {
            SDL_SetRenderDrawColor(_renderer, _isDraggingH ? 120 : 80, _isDraggingH ? 120 : 80, _isDraggingH ? 120 : 80, 255);
            SDL_RenderFillRect(_renderer, &_hThumb);
        }
        
        if (_textHeight > _textArea.h - SCROLLBAR_SIZE) {
            SDL_SetRenderDrawColor(_renderer, _isDraggingV ? 120 : 80, _isDraggingV ? 120 : 80, _isDraggingV ? 120 : 80, 255);
            SDL_RenderFillRect(_renderer, &_vThumb);
        }
    }

    void selectAllText() {
        if (_fileContent.empty()) return;
        _selStartLine = 0;
        _selStartCol = 0;
        _selEndLine = _fileContent.size() - 1;
        _selEndCol = _fileContent.back().length();
        _change = true;
    }

    void processFileLoad(const std::string& filePath) {
        std::fstream file;
        file.open(filePath, std::ios::in);

        if (!file.is_open()) {
            SDL_Log("Failed to open the file: %s", filePath.c_str());
            return;
        }

        _currentFilePath = filePath;

        _fileContent.clear();
        std::string line;
        while (std::getline(file, line)) {
            _fileContent.push_back(line);
        }
        file.close();
        
        if (_fileContent.empty()) {
            _fileContent.push_back("");
        }

        clearSelection();
        _cursorLine = 0;
        _cursorCol = 0;
        _scrollOffsetX = 0.0f;
        _scrollOffsetY = 0.0f;

        updateTextDisplay();
        
        _isDirty = false;
        requestAiSuggestion();
    }

    void updateTextDisplay() {
        clearRenderLines();

        if (_fileContent.empty()) {
            _textWidth = 0;
            _textHeight = 0;
            return;
        }

        int maxLines = _fileContent.size();
        int padWidth = std::to_string(maxLines).length();
        int lineHeight = TTF_GetFontHeight(_font);
        if (lineHeight <= 0) lineHeight = 24;
        
        _textWidth = 0;
        
        for (size_t i = 0; i < _fileContent.size(); ++i) {
            RenderLine rl;
            
            std::string numStr = std::to_string(i + 1);
            std::string padding(padWidth - numStr.length(), ' ');
            std::string numDisplay = padding + numStr + "  ";

            rl.numText = TTF_CreateText(_textEngine, _font, numDisplay.c_str(), numDisplay.length());
            if (rl.numText) {
                TTF_GetTextSize(rl.numText, &rl.numW, &rl.numH);
                TTF_SetTextColor(rl.numText, 0, 255, 232, 255);
            }

            std::string contentDisplay = _fileContent[i];
            if (contentDisplay.empty()) contentDisplay = " ";

            rl.contentText = TTF_CreateText(_textEngine, _font, contentDisplay.c_str(), contentDisplay.length());
            if (rl.contentText) {
                TTF_GetTextSize(rl.contentText, &rl.contentW, &rl.contentH);
                TTF_SetTextColor(rl.contentText, 22, 223, 45, 255);
            }

            int totalLineWidth = rl.numW + rl.contentW;
            if (totalLineWidth > _textWidth) {
                _textWidth = totalLineWidth;
            }
            
            _renderLines.push_back(rl);
        }

        _textHeight = _renderLines.size() * lineHeight;
        clampAndUpdateScrollbars();
    }

    void getLineAndColFromMouse(float mouseX, float mouseY, int& lineOut, int& colOut) {
        if (_fileContent.empty()) {
            lineOut = 0; colOut = 0; return;
        }

        int lineHeight = TTF_GetFontHeight(_font);
        if (lineHeight <= 0) lineHeight = 24;

        float relativeY = mouseY - (_textArea.y + 5.0f) + _scrollOffsetY;
        lineOut = (int)(relativeY / lineHeight);
        lineOut = std::clamp(lineOut, 0, (int)_fileContent.size() - 1);

        float startX = _textArea.x + 5.0f - _scrollOffsetX + _renderLines[lineOut].numW;
        float relativeX = mouseX - startX;

        if (relativeX <= 0.0f) {
            colOut = 0;
            return;
        }

        const std::string& lineStr = _fileContent[lineOut];
        
        if (_charWidth > 0) {
            colOut = (int)((relativeX + (_charWidth / 2.0f)) / _charWidth);
        } else {
            colOut = 0;
        }

        colOut = std::clamp(colOut, 0, (int)lineStr.length());
    }

    void copySelectionToClipboard() {
        if (_selStartLine == -1 || _selEndLine == -1 || _fileContent.empty()) return;

        int sL = _selStartLine, sC = _selStartCol;
        int eL = _selEndLine, eC = _selEndCol;
        if (sL > eL || (sL == eL && sC > eC)) {
            std::swap(sL, eL);
            std::swap(sC, eC);
        }

        std::string copiedText = "";
        for (int i = sL; i <= eL; ++i) {
            int col1 = (i == sL) ? sC : 0;
            int col2 = (i == eL) ? eC : _fileContent[i].length();
            
            copiedText += _fileContent[i].substr(col1, col2 - col1);
            if (i < eL) copiedText += "\n";
        }

        if (!copiedText.empty()) {
            SDL_SetClipboardText(copiedText.c_str());
        }
    }

    static void SDLCALL loadFileCallback(void *userData, const char* const* fileList, int filter){
        if (!fileList || !*fileList) {
            return;
        }

        Editor* editor = static_cast<Editor*>(userData);
        {
            std::lock_guard<std::mutex> lock(editor->_fileMutex);
            editor->_pendingFilePath = *fileList;
        }
        editor->_isFileReady = true;
    }

    static void SDLCALL saveFileCallback(void *userData, const char* const* fileList, int filter){
        if (!fileList || !*fileList) {
            return;
        }

        Editor* editor = static_cast<Editor*>(userData);
        {
            std::lock_guard<std::mutex> lock(editor->_fileMutex);
            editor->_pendingSavePath = *fileList;
        }
        editor->_isSaveReady = true;
    }

    static void SDLCALL loadFolderCallback(void *userData, const char* const* folderList, int filter){
        if (!folderList || !*folderList) {
            return;
        }

        Editor* editor = static_cast<Editor*>(userData);
        {
            std::lock_guard<std::mutex> lock(editor->_folderMutex);
            editor->_pendingFolderPath = *folderList;
        }
        editor->_isFolderReady = true;
    }

    static SDL_EnumerationResult SDLCALL DirEnumCallback(void *userdata, const char *dirname, const char *fname) {
        Folder* parentFolder = static_cast<Folder*>(userdata);

        std::string fileName = fname;
        if (fileName == "." || fileName == "..") {
            return SDL_ENUM_CONTINUE;
        }

        std::string dirStr = dirname;
        if (!dirStr.empty() && dirStr.back() != '/' && dirStr.back() != '\\') {
            dirStr += "/";
        }
        
        std::string fullPath = dirStr + fname;
        SDL_PathInfo info;
        
        if (SDL_GetPathInfo(fullPath.c_str(), &info)) {
            if (info.type == SDL_PATHTYPE_DIRECTORY) {
                Folder* child = new Folder();
                child->path = fullPath;
                child->name = fileName;
                child->isExpanded = false;
                parentFolder->childFolders.push_back(child);
            } else {
                parentFolder->files.push_back(fileName);
            }
        }
        return SDL_ENUM_CONTINUE;
    }
};

int main(int argc, char* argv[]) {
    Editor editor;
    editor.runEditor();
    return 0;
}