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
#include <cmath>
#include <sstream>

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

        _fileContent.push_back("");
        updateTextDisplay();

        SDL_StartTextInput(_window);
        updateDimensionsOnResize();
        _state = true;
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
                saveFile(); 
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
        clearRenderLines();
        if (_textEngine) TTF_DestroyRendererTextEngine(_textEngine);
        if (_font) TTF_CloseFont(_font);
        if (_renderer) SDL_DestroyRenderer(_renderer);
        if (_window) SDL_DestroyWindow(_window);
        TTF_Quit();
        SDL_Quit();
    }

private:
    void clearRenderLines() {
        for (auto& rl : _renderLines) {
            if (rl.numText) TTF_DestroyText(rl.numText);
            if (rl.contentText) TTF_DestroyText(rl.contentText);
        }
        _renderLines.clear();
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
            clampAndUpdateScrollbars();
            _change = true;
        }
        else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
            _scrollOffsetY -= event.wheel.y * 40.0f;
            _scrollOffsetX -= event.wheel.x * 40.0f;
            clampAndUpdateScrollbars();
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
                _isSelectingText = false;
                _change = true;
            }
        }
        else if (event.type == SDL_EVENT_MOUSE_MOTION) {
            handleMouseMotion(event.motion.x, event.motion.y);
        }
    }

    void handleTextInput(const char* text) {
        clearSelection();
        _fileContent[_cursorLine].insert(_cursorCol, text);
        _cursorCol += strlen(text);
        
        updateSingleRenderLine(_cursorLine);
        forceCursorVisible();
        _change = true;
    }

    void handleKeyDown(const SDL_KeyboardEvent& key) {
        bool ctrlPressed = (key.mod & SDL_KMOD_CTRL);

        if (key.key == SDLK_ESCAPE) {
            _state = false;
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
        }
        else if (key.key == SDLK_UP) {
            clearSelection();
            if (_cursorLine > 0) {
                _cursorLine--;
                _cursorCol = std::min(_cursorCol, (int)_fileContent[_cursorLine].length());
            }
            forceCursorVisible();
        }
        else if (key.key == SDLK_DOWN) {
            clearSelection();
            if (_cursorLine < _fileContent.size() - 1) {
                _cursorLine++;
                _cursorCol = std::min(_cursorCol, (int)_fileContent[_cursorLine].length());
            }
            forceCursorVisible();
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
    }

    void saveFile() {
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

        if (SDL_PointInRectFloat(&clickPoint, &_vThumb)) {
            _isDraggingV = true;
            _dragOffsetY = mouseY - _vThumb.y;
            return;
        }
        if (SDL_PointInRectFloat(&clickPoint, &_hThumb)) {
            _isDraggingH = true;
            _dragOffsetX = mouseX - _hThumb.x;
            return;
        }

        for (const auto& hb : _hitBoxes) {
            if (SDL_PointInRectFloat(&clickPoint, &hb.box)) {
                if (hb.name == "FileText") {
                    SDL_ShowOpenFileDialog(Editor::loadFileCallback, this, _window, nullptr, 0, nullptr, false);
                    return; 
                }
            }
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

    void updateDimensionsOnResize() {
        SDL_Rect displayBounds;
        SDL_GetWindowSize(_window, &displayBounds.w, &displayBounds.h);
        
        _width = displayBounds.w;
        _height = displayBounds.h;

        _titleBar = { 0.0f, 0.0f, (float)_width, 40.0f };
        _sideBar = { 0.0f, 40.0f, (float)_width * 0.2f, (float)_height - 40.0f };
        _textArea = { (float)_width * 0.2f, 40.0f, (float)_width * 0.8f, (float)_height - 40.0f };

        updateHitboxes();
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
            TTF_DrawRendererText(fileOptions, 0, 0);
            TTF_DestroyText(fileOptions);
        }
    }

    void drawSideBar() {
        SDL_SetRenderDrawColor(_renderer, 30, 30, 30, 255);
        SDL_RenderFillRect(_renderer, &_sideBar);
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

        if (!_renderLines.empty()) {
            int firstVisibleLine = std::max(0, (int)(_scrollOffsetY / lineHeight));
            int visibleLineCount = std::ceil(clipRect.h / (float)lineHeight) + 1;
            int lastVisibleLine = std::min((int)_renderLines.size(), firstVisibleLine + visibleLineCount);

            int sL = _selStartLine, sC = _selStartCol;
            int eL = _selEndLine, eC = _selEndCol;
            
            bool hasSelection = (sL != -1 && eL != -1) && (sL != eL || sC != eC);
            if (hasSelection && (sL > eL || (sL == eL && sC > eC))) {
                std::swap(sL, eL);
                std::swap(sC, eC);
            }

            for (int i = firstVisibleLine; i < lastVisibleLine; ++i) {
                float yPos = _textArea.y + 5.0f - _scrollOffsetY + (i * lineHeight);

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
};

int main(int argc, char* argv[]) {
    Editor editor;
    editor.runEditor();
    return 0;
}