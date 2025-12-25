
#include <imgui.h>
#include <backends/imgui_impl_sdl2.h>
#include <backends/imgui_impl_opengl2.h>

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_opengl.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>




namespace ws {

struct ProductConfig {
    std::string name = "Product";
    std::string unitName = "pcs";
    int   unitsPerPackage   = 10;
    int   capacityPackages  = 20;
    int   shelfLifeDays     = 14;
    int   discountBeforeDays= 3;
    float basePricePerUnit  = 1.0f;
    float discountPercent   = 25.0f;
    int   reorderThreshold  = 6;
    int   reorderAmount     = 12;
};

struct DayStats {
    double revenue = 0.0;
    double discountLoss = 0.0;
    double writeOffLoss = 0.0;
    int totalSoldUnits = 0;
    int totalWriteOffUnits = 0;
};

struct SimConfig {
    int N = 20;
    int M = 5;
    int K = 12;
    uint32_t seed = 12345;

    float orderProbability = 0.85f;
    int maxLinesPerOrder = 6;
    int minUnitsPerLine = 5;
    int maxUnitsPerLine = 120;

    float discountDemandBoost = 2.0f;

    int supplierLeadTimeMin = 1;
    int supplierLeadTimeMax = 5;

    float secondsPerDay = 6.0f;
};

struct OrderRow {
    int shopId = -1;
    int productId = -1;
    int requestedUnits = 0;
    int chosenPackages = 0;
    int deliveredPackages = 0;
    int deliveredUnits = 0;
};

enum class AnimType { SupplyTruck, ShipmentTruck, OrderPaper, WasteTruck };

struct Anim {
    AnimType type{};
    float t0 = 0.f;
    float t1 = 1.f;
    int shopId = -1;
    int payload = 0;
};

struct SimulatorHandle;

SimulatorHandle* CreateSimulator(const SimConfig& cfg);
void DestroySimulator(SimulatorHandle* h);

void ResetSimulator(SimulatorHandle* h, const SimConfig& cfg);
void AdvanceDay(SimulatorHandle* h);

int GetDay(const SimulatorHandle* h);
SimConfig GetConfigCopy(const SimulatorHandle* h);

void SetRuntimeTuning(SimulatorHandle* h,
                      float orderProbability,
                      int maxLinesPerOrder,
                      int minUnitsPerLine,
                      int maxUnitsPerLine,
                      float discountDemandBoost,
                      float secondsPerDay);

DayStats GetTodaysStats(const SimulatorHandle* h);
DayStats GetTotalStats(const SimulatorHandle* h);

void GetShopNames(const SimulatorHandle* h, std::vector<std::string>& out);

void GetProductConfigs(const SimulatorHandle* h, std::vector<ProductConfig>& out);
void SetProductConfigs(SimulatorHandle* h, const std::vector<ProductConfig>& in);

int  GetStockPackages(const SimulatorHandle* h, int pid);
bool HasDiscountStock(const SimulatorHandle* h, int pid);

void GetTodaysOrderRows(const SimulatorHandle* h, std::vector<OrderRow>& out);
void GetLog(const SimulatorHandle* h, std::vector<std::string>& out, int maxLines);

void GetDailyAnims(const SimulatorHandle* h, std::vector<Anim>& out);

} 




static float Clamp01(float x) { return (x < 0.f) ? 0.f : (x > 1.f ? 1.f : x); }

static std::string FmtMoney(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return std::string(buf);
}

struct UIState {
    bool running = false;
    float dayProgress = 0.f;
    float speed = 1.0f;
};




static ImVec2 Lerp(const ImVec2& a, const ImVec2& b, float t) {
    t = Clamp01(t);
    return ImVec2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
}

static void DrawSimulationCanvas(int day, int N,
                                 const std::vector<std::string>& shopNames,
                                 const std::vector<ws::Anim>& anims,
                                 UIState& ui)
{
    ImGui::BeginChild("Canvas", ImVec2(0, 0), true, ImGuiWindowFlags_NoScrollbar);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 sz = ImGui::GetContentRegionAvail();
    ImVec2 p1 = ImVec2(p0.x + sz.x, p0.y + sz.y);

    dl->AddRectFilled(p0, p1, IM_COL32(18, 20, 24, 255));
    dl->AddRect(p0, p1, IM_COL32(70, 80, 95, 255), 0.f, 0, 2.f);

    ImVec2 warehouseC = ImVec2(p0.x + sz.x * 0.25f, p0.y + sz.y * 0.50f);
    ImVec2 supplierC  = ImVec2(p0.x + sz.x * 0.08f, p0.y + sz.y * 0.18f);
    ImVec2 dumpC      = ImVec2(p0.x + sz.x * 0.10f, p0.y + sz.y * 0.86f);

    float Ww = sz.x * 0.22f;
    float Wh = sz.y * 0.28f;
    ImVec2 w0 = ImVec2(warehouseC.x - Ww*0.5f, warehouseC.y - Wh*0.5f);
    ImVec2 w1 = ImVec2(warehouseC.x + Ww*0.5f, warehouseC.y + Wh*0.5f);

    dl->AddRectFilled(w0, w1, IM_COL32(34, 40, 48, 255), 10.f);
    dl->AddRect(w0, w1, IM_COL32(200, 200, 220, 255), 10.f, 0, 2.f);
    dl->AddText(ImVec2(w0.x + 10, w0.y + 10), IM_COL32(230, 230, 240, 255), "Склад");

    float R = std::min(sz.x, sz.y) * 0.035f;

    dl->AddCircleFilled(supplierC, R, IM_COL32(50, 60, 80, 255));
    dl->AddCircle(supplierC, R, IM_COL32(200, 220, 255, 255), 0, 2.f);
    dl->AddText(ImVec2(supplierC.x - R, supplierC.y + R + 2), IM_COL32(220, 220, 240, 255), "Поставщик");

    dl->AddCircleFilled(dumpC, R, IM_COL32(70, 45, 45, 255));
    dl->AddCircle(dumpC, R, IM_COL32(255, 210, 210, 255), 0, 2.f);
    dl->AddText(ImVec2(dumpC.x - R, dumpC.y + R + 2), IM_COL32(230, 230, 240, 255), "Вывоз");

    int M = (int)shopNames.size();
    std::vector<ImVec2> shopPos(M);
    ImVec2 shopsCenter = ImVec2(p0.x + sz.x * 0.75f, p0.y + sz.y * 0.50f);
    float rad = std::min(sz.x, sz.y) * 0.33f;

    for (int i = 0; i < M; ++i) {
        float ang = (float)i / std::max(1, M) * 2.0f * 3.1415926f - 0.6f;
        shopPos[i] = ImVec2(shopsCenter.x + std::cos(ang) * rad, shopsCenter.y + std::sin(ang) * rad);

        dl->AddCircleFilled(shopPos[i], R, IM_COL32(40, 70, 45, 255));
        dl->AddCircle(shopPos[i], R, IM_COL32(210, 255, 220, 255), 0, 2.f);
        dl->AddText(ImVec2(shopPos[i].x - R, shopPos[i].y + R + 2), IM_COL32(230, 230, 240, 255), shopNames[i].c_str());
        dl->AddLine(warehouseC, shopPos[i], IM_COL32(60, 70, 85, 140), 2.f);
    }

    dl->AddLine(supplierC, warehouseC, IM_COL32(60, 70, 85, 140), 2.f);
    dl->AddLine(warehouseC, dumpC, IM_COL32(60, 70, 85, 140), 2.f);

    
    {
        ImVec2 bar0 = ImVec2(p0.x + 12, p0.y + 12);
        ImVec2 bar1 = ImVec2(p0.x + sz.x - 12, p0.y + 30);
        dl->AddRectFilled(bar0, bar1, IM_COL32(30, 35, 42, 255), 6.f);
        ImVec2 fill1 = ImVec2(bar0.x + (bar1.x - bar0.x) * Clamp01(ui.dayProgress), bar1.y);
        dl->AddRectFilled(bar0, fill1, IM_COL32(120, 170, 255, 180), 6.f);

        std::string t = "День " + std::to_string(day) + "/" + std::to_string(N) +
                        "   (" + std::to_string((int)std::round(ui.dayProgress*100)) + "%)";
        dl->AddText(ImVec2(bar0.x + 8, bar0.y + 2), IM_COL32(240, 240, 250, 255), t.c_str());
    }

    auto iconRect = [&](ImVec2 c, float s)->std::pair<ImVec2,ImVec2>{
        return {ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s)};
    };

    for (auto& a : anims) {
        if (ui.dayProgress < a.t0 || ui.dayProgress > a.t1) continue;
        float t = (ui.dayProgress - a.t0) / std::max(0.0001f, (a.t1 - a.t0));

        ImVec2 start = warehouseC, end = warehouseC;
        ImU32 col = IM_COL32(255,255,255,255);

        if (a.type == ws::AnimType::SupplyTruck) {
            start = supplierC; end = warehouseC; col = IM_COL32(130, 200, 255, 255);
        } else if (a.type == ws::AnimType::WasteTruck) {
            start = warehouseC; end = dumpC; col = IM_COL32(255, 160, 160, 255);
        } else if (a.type == ws::AnimType::OrderPaper) {
            if (a.shopId < 0 || a.shopId >= M) continue;
            start = shopPos[a.shopId]; end = warehouseC; col = IM_COL32(255, 235, 150, 255);
        } else if (a.type == ws::AnimType::ShipmentTruck) {
            if (a.shopId < 0 || a.shopId >= M) continue;
            start = warehouseC; end = shopPos[a.shopId]; col = IM_COL32(170, 255, 190, 255);
        }

        ImVec2 pos = Lerp(start, end, t);
        dl->AddLine(start, pos, IM_COL32(255,255,255,45), 3.f);

        float s = R * 0.60f;
        auto [r0, r1] = iconRect(pos, s);

        if (a.type == ws::AnimType::OrderPaper) {
            dl->AddRectFilled(r0, r1, col, 4.f);
            dl->AddRect(r0, r1, IM_COL32(20,20,20,120), 4.f, 0, 1.5f);
        } else {
            dl->AddRectFilled(r0, r1, IM_COL32(25, 25, 30, 255), 4.f);
            dl->AddRect(r0, r1, col, 4.f, 0, 2.f);
            dl->AddLine(ImVec2(r0.x, r1.y), ImVec2(r1.x, r1.y), col, 2.f);
        }

        if (a.payload > 0) {
            std::string txt = std::to_string(a.payload);
            dl->AddText(ImVec2(pos.x + s + 4, pos.y - s), IM_COL32(240,240,250,220), txt.c_str());
        }
    }

    ImGui::EndChild();
}




static void ProductEditor(ws::SimulatorHandle* sim,
                          std::vector<ws::ProductConfig>& products)
{
    if (ImGui::BeginTable("products", 9,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                          ImVec2(0, 240))) {

        ImGui::TableSetupColumn("Товар");
        ImGui::TableSetupColumn("Упак(ед.)");
        ImGui::TableSetupColumn("Ёмк(уп.)");
        ImGui::TableSetupColumn("Срок(дн.)");
        ImGui::TableSetupColumn("Уценка до(дн.)");
        ImGui::TableSetupColumn("Цена/ед");
        ImGui::TableSetupColumn("Уценка %");
        ImGui::TableSetupColumn("Остаток(уп.)");
        ImGui::TableSetupColumn("Есть уценка?");
        ImGui::TableHeadersRow();

        for (int pid = 0; pid < (int)products.size(); ++pid) {
            auto& p = products[pid];

            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(p.name.c_str());

            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputInt(("##u"+std::to_string(pid)).c_str(), &p.unitsPerPackage);

            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputInt(("##cap"+std::to_string(pid)).c_str(), &p.capacityPackages);

            ImGui::TableSetColumnIndex(3);
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputInt(("##life"+std::to_string(pid)).c_str(), &p.shelfLifeDays);

            ImGui::TableSetColumnIndex(4);
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputInt(("##db"+std::to_string(pid)).c_str(), &p.discountBeforeDays);

            ImGui::TableSetColumnIndex(5);
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputFloat(("##price"+std::to_string(pid)).c_str(), &p.basePricePerUnit, 0.f, 0.f, "%.2f");

            ImGui::TableSetColumnIndex(6);
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::SliderFloat(("##disc"+std::to_string(pid)).c_str(), &p.discountPercent, 0.f, 80.f, "%.0f%%");

            ImGui::TableSetColumnIndex(7);
            ImGui::Text("%d", ws::GetStockPackages(sim, pid));

            ImGui::TableSetColumnIndex(8);
            ImGui::TextUnformatted(ws::HasDiscountStock(sim, pid) ? "да" : "нет");
        }

        ImGui::EndTable();
    }
}

static void OrdersTable(const std::vector<ws::OrderRow>& rows,
                        const std::vector<ws::ProductConfig>& products)
{
    if (ImGui::BeginTable("orders", 6,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                          ImVec2(0, 220))) {
        ImGui::TableSetupColumn("Точка");
        ImGui::TableSetupColumn("Товар");
        ImGui::TableSetupColumn("Запрос(ед)");
        ImGui::TableSetupColumn("Выбрано(уп)");
        ImGui::TableSetupColumn("Выдано(уп)");
        ImGui::TableSetupColumn("Выдано(ед)");
        ImGui::TableHeadersRow();

        for (auto& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", r.shopId + 1);

            ImGui::TableSetColumnIndex(1);
            const char* nm = (r.productId >= 0 && r.productId < (int)products.size())
                ? products[r.productId].name.c_str()
                : "?";
            ImGui::TextUnformatted(nm);

            ImGui::TableSetColumnIndex(2); ImGui::Text("%d", r.requestedUnits);
            ImGui::TableSetColumnIndex(3); ImGui::Text("%d", r.chosenPackages);
            ImGui::TableSetColumnIndex(4); ImGui::Text("%d", r.deliveredPackages);
            ImGui::TableSetColumnIndex(5); ImGui::Text("%d", r.deliveredUnits);
        }

        ImGui::EndTable();
    }
}

static void LogView(const std::vector<std::string>& log) {
    ImGui::BeginChild("log", ImVec2(0, 180), true);
    for (auto& s : log) ImGui::TextUnformatted(s.c_str());
    ImGui::EndChild();
}




int main(int, char**) {
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        SDL_Log("SDL_Init Error: %s", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);

    SDL_Window* window = SDL_CreateWindow(
        "Wholesale Warehouse Simulation (ImGui)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 720,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE
    );

    if (!window) {
        SDL_Log("CreateWindow Error: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1);

    
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    (void)io;

    
    const ImWchar* ranges = io.Fonts->GetGlyphRangesCyrillic();
    ImFont* font = io.Fonts->AddFontFromFileTTF("arial.ttf", 18.0f, nullptr, ranges);
    if (!font) io.Fonts->AddFontDefault();

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 10.f;
    style.FrameRounding = 8.f;
    style.ScrollbarRounding = 10.f;
    style.GrabRounding = 10.f;

    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL2_Init();
    ImGui_ImplOpenGL2_CreateFontsTexture();

    
    ws::SimConfig initCfg;
    initCfg.N = 20; initCfg.M = 5; initCfg.K = 12;
    initCfg.seed = 12345;

    ws::SimulatorHandle* sim = ws::CreateSimulator(initCfg);
    UIState ui;

    
    ws::SimConfig pendingCfg = ws::GetConfigCopy(sim);

    bool done = false;
    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) done = true;
            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_CLOSE &&
                event.window.windowID == SDL_GetWindowID(window)) done = true;
        }

        
        ws::SimConfig liveCfg = ws::GetConfigCopy(sim);
        int day = ws::GetDay(sim);

        if (ui.running && day < liveCfg.N) {
            float secondsPerDay = std::max(0.25f, liveCfg.secondsPerDay / std::max(0.1f, ui.speed));
            ui.dayProgress += (io.DeltaTime / secondsPerDay);
            if (ui.dayProgress >= 1.f) {
                ui.dayProgress = 0.f;
                ws::AdvanceDay(sim);
                day = ws::GetDay(sim);
                if (day >= liveCfg.N) ui.running = false;
            }
        }

        
        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        
        std::vector<std::string> shopNames;
        std::vector<ws::ProductConfig> products;
        std::vector<ws::OrderRow> orderRows;
        std::vector<std::string> logLines;
        std::vector<ws::Anim> anims;

        ws::GetShopNames(sim, shopNames);
        ws::GetProductConfigs(sim, products);
        ws::GetTodaysOrderRows(sim, orderRows);
        ws::GetLog(sim, logLines, 200);
        ws::GetDailyAnims(sim, anims);

        ws::DayStats todays = ws::GetTodaysStats(sim);
        ws::DayStats total = ws::GetTotalStats(sim);

        
        ImGui::Begin("Управление симуляцией");

        ImGui::Text("Дата: день %d из %d", day, liveCfg.N);
        ImGui::Separator();

        if (ImGui::Button(ui.running ? "Пауза" : "Старт")) {
            if (day == 0 && !ui.running) ws::AdvanceDay(sim);
            ui.running = !ui.running;
        }
        ImGui::SameLine();
        if (ImGui::Button("Шаг (1 день)")) {
            ui.running = false;
            if (ws::GetDay(sim) < liveCfg.N) {
                ui.dayProgress = 0.f;
                ws::AdvanceDay(sim);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Сброс")) {
            ui.running = false;
            ui.dayProgress = 0.f;
            ws::ResetSimulator(sim, pendingCfg);
            pendingCfg = ws::GetConfigCopy(sim);
        }

        ImGui::SameLine();

        ImGui::SliderFloat("Скорость", &ui.speed, 0.25f, 4.0f, "%.2fx");

        
        bool tuneChanged = false;
        tuneChanged |= ImGui::SliderFloat("P(заказ в день от точки)", &liveCfg.orderProbability, 0.0f, 1.0f, "%.2f");
        tuneChanged |= ImGui::InputInt("Max позиций в заказе", &liveCfg.maxLinesPerOrder);
        tuneChanged |= ImGui::InputInt("Мин ед. в строке", &liveCfg.minUnitsPerLine);
        tuneChanged |= ImGui::InputInt("Макс ед. в строке", &liveCfg.maxUnitsPerLine);
        tuneChanged |= ImGui::SliderFloat("Усиление спроса от уценки", &liveCfg.discountDemandBoost, 0.0f, 5.0f, "%.2f");
        tuneChanged |= ImGui::SliderFloat("Секунд на день", &liveCfg.secondsPerDay, 1.5f, 14.0f, "%.1f");

        if (tuneChanged) {
            ws::SetRuntimeTuning(sim,
                                 liveCfg.orderProbability,
                                 liveCfg.maxLinesPerOrder,
                                 liveCfg.minUnitsPerLine,
                                 liveCfg.maxUnitsPerLine,
                                 liveCfg.discountDemandBoost,
                                 liveCfg.secondsPerDay);
            liveCfg = ws::GetConfigCopy(sim);
        }

        ImGui::Separator();
        ImGui::Text("Параметры (вступают в силу после 'Сброс')");

        ImGui::InputInt("N (дней)", &pendingCfg.N);
        ImGui::InputInt("M (точек)", &pendingCfg.M);
        ImGui::InputInt("K (товаров)", &pendingCfg.K);
        ImGui::InputScalar("Seed", ImGuiDataType_U32, &pendingCfg.seed);

        pendingCfg.N = std::clamp(pendingCfg.N, 10, 30);
        pendingCfg.M = std::clamp(pendingCfg.M, 3, 9);
        pendingCfg.K = std::clamp(pendingCfg.K, 12, 20);

        ImGui::Separator();
        ImGui::Text("Итоги сегодня:");
        ImGui::BulletText("Выручка: %s", FmtMoney(todays.revenue).c_str());
        ImGui::BulletText("Потери (уценка): %s", FmtMoney(todays.discountLoss).c_str());
        ImGui::BulletText("Потери (списание): %s", FmtMoney(todays.writeOffLoss).c_str());
        ImGui::BulletText("Продано (ед.): %d", todays.totalSoldUnits);
        ImGui::BulletText("Списано (ед.): %d", todays.totalWriteOffUnits);

        ImGui::Text("Итоги за период:");
        ImGui::BulletText("Выручка: %s", FmtMoney(total.revenue).c_str());
        ImGui::BulletText("Потери (уценка): %s", FmtMoney(total.discountLoss).c_str());
        ImGui::BulletText("Потери (списание): %s", FmtMoney(total.writeOffLoss).c_str());
        ImGui::BulletText("Продано (ед.): %d", total.totalSoldUnits);
        ImGui::BulletText("Списано (ед.): %d", total.totalWriteOffUnits);

        ImGui::End();

        
        ImGui::Begin("Склад: товары/настройка уценки (заведующий)");
        ProductEditor(sim, products);
        
        ws::SetProductConfigs(sim, products);
        ImGui::End();

        
        ImGui::Begin("Заказы текущего дня (с обработкой упаковками)");
        OrdersTable(orderRows, products);
        ImGui::End();

        
        ImGui::Begin("Лог / события");
        LogView(logLines);
        ImGui::End();

        
        ImGui::Begin("Визуализация (движение процессов)");
        ws::SimConfig cfgNow = ws::GetConfigCopy(sim);
        int dayNow = ws::GetDay(sim);
        DrawSimulationCanvas(dayNow, cfgNow.N, shopNames, anims, ui);
        ImGui::End();

        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.08f, 0.09f, 0.10f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    ws::DestroySimulator(sim);

    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
