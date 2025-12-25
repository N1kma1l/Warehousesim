#include <imgui.h>
#include <backends/imgui_impl_sdl2.h>
#include <backends/imgui_impl_opengl2.h>

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_opengl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <map>
#include <numeric>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>




static float Clamp01(float x) { return (x < 0.f) ? 0.f : (x > 1.f ? 1.f : x); }

static ImVec2 Lerp(const ImVec2& a, const ImVec2& b, float t) {
    t = Clamp01(t);
    return ImVec2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
}

static std::string FmtMoney(double v) {
    std::ostringstream ss;
    ss.setf(std::ios::fixed);
    ss << std::setprecision(2) << v;
    return ss.str();
}

template <class T>
static T RandInt(std::mt19937& rng, T lo, T hi) {
    std::uniform_int_distribution<T> d(lo, hi);
    return d(rng);
}

static float RandFloat(std::mt19937& rng, float lo, float hi) {
    std::uniform_real_distribution<float> d(lo, hi);
    return d(rng);
}

static bool RandBernoulli(std::mt19937& rng, float p) {
    p = Clamp01(p);
    std::bernoulli_distribution d(p);
    return d(rng);
}




struct ProductConfig {
    std::string name = "Product";
    std::string unitName = "pcs";   
    int unitsPerPackage = 10;       
    int capacityPackages = 20;      
    int shelfLifeDays = 14;         
    int discountBeforeDays = 3;     
    float basePricePerUnit = 1.0f;  
    float discountPercent = 25.0f;  
    int reorderThreshold = 6;       
    int reorderAmount = 12;         
};

struct Batch {
    int packages = 0;
    int daysLeft = 0;        
    bool discounted = false; 
    float unitPrice = 0.0f;  
};

struct SupplierDelivery {
    int productId = -1;
    int packages = 0;
    int daysUntilArrival = 0; 
};

struct OrderLine {
    int productId = -1;
    int requestedUnits = 0;
    int chosenPackages = 0;     
    int deliveredPackages = 0;  
    int deliveredUnits = 0;
};

struct Order {
    int day = 0;
    int shopId = -1;
    std::vector<OrderLine> lines;
};

struct Shipment {
    int day = 0;        
    int shopId = -1;
    
    std::vector<std::pair<int,int>> items;
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

struct Warehouse {
    std::vector<std::deque<Batch>> inv; 
    std::vector<int> reservedPackages;  

    explicit Warehouse(int K=0) {
        inv.resize(K);
        reservedPackages.assign(K, 0);
    }

    int TotalPackages(int pid) const {
        int s = 0;
        for (auto& b : inv[pid]) s += b.packages;
        return s;
    }

    bool HasDiscountStock(int pid) const {
        for (auto& b : inv[pid]) if (b.packages > 0 && b.discounted) return true;
        return false;
    }

    
    void DecrementShelfLife() {
        for (auto& dq : inv) {
            for (auto& b : dq) b.daysLeft -= 1;
        }
    }

    
    int WriteOffExpired(int pid, const ProductConfig& pc, DayStats& st) {
        int writeOffPackages = 0;
        auto& dq = inv[pid];
        while (!dq.empty() && dq.front().daysLeft <= 0) {
            writeOffPackages += dq.front().packages;
            dq.pop_front();
        }
        if (writeOffPackages > 0) {
            int units = writeOffPackages * pc.unitsPerPackage;
            st.totalWriteOffUnits += units;
            st.writeOffLoss += double(units) * double(pc.basePricePerUnit);
        }
        return writeOffPackages;
    }

    
    void Receive(int pid, int packages, const ProductConfig& pc) {
        if (packages <= 0) return;
        Batch b;
        b.packages = packages;
        b.daysLeft = pc.shelfLifeDays;
        b.discounted = false;
        b.unitPrice = pc.basePricePerUnit;
        inv[pid].push_back(b);
    }

    
    
    int ApplyDiscountIfNeeded(int pid, const ProductConfig& pc) {
        int affectedPackages = 0;
        for (auto& b : inv[pid]) {
            if (!b.discounted && b.daysLeft > 0 && b.daysLeft <= pc.discountBeforeDays) {
                b.discounted = true;
                b.unitPrice = pc.basePricePerUnit * (1.0f - pc.discountPercent / 100.0f);
                affectedPackages += b.packages;
            }
        }
        return affectedPackages;
    }

    
    
    
    
    
    void FulfillLine(OrderLine& line,
                     const ProductConfig& pc,
                     std::mt19937& rng,
                     DayStats& st)
    {
        if (line.requestedUnits <= 0) return;

        const float exact = float(line.requestedUnits) / float(pc.unitsPerPackage);
        int fl = (int)std::floor(exact);
        int ce = (int)std::ceil(exact);

        
        int chosen = ce;
        if (fl == ce) chosen = fl;
        else {
            float errFl = std::fabs(exact - float(fl));
            float errCe = std::fabs(float(ce) - exact);
            if (errFl < errCe) chosen = fl;
            else if (errCe < errFl) chosen = ce;
            else chosen = RandBernoulli(rng, 0.5f) ? fl : ce;
        }

        
        
        if (RandBernoulli(rng, 0.35f)) {
            int delta = RandBernoulli(rng, 0.5f) ? -1 : +1;
            chosen = std::max(0, chosen + delta);
        }
        if (chosen == 0) chosen = 1;

        line.chosenPackages = chosen;

        int available = TotalPackages(line.productId);
        int give = std::min(chosen, available);
        line.deliveredPackages = give;
        line.deliveredUnits = give * pc.unitsPerPackage;

        
        
        
        int remaining = give;
        auto& dq = inv[line.productId];
        while (remaining > 0 && !dq.empty()) {
            Batch& b = dq.front();
            int take = std::min(remaining, b.packages);
            b.packages -= take;
            remaining -= take;

            const int units = take * pc.unitsPerPackage;
            const double base = double(units) * double(pc.basePricePerUnit);
            const double actual = double(units) * double(b.unitPrice);

            st.revenue += actual;
            st.totalSoldUnits += units;

            
            if (b.discounted) {
                st.discountLoss += (base - actual);
            }

            if (b.packages == 0) dq.pop_front();
        }
    }
};




enum class AnimType { SupplyTruck, ShipmentTruck, OrderPaper, WasteTruck };

struct Anim {
    AnimType type{};
    ImVec2 start{}, end{};
    float t0 = 0.f;   
    float t1 = 1.f;
    int shopId = -1;
    int payload = 0;  
};

struct UIState {
    bool running = false;
    float dayProgress = 0.f;     
    float speed = 1.0f;          
    bool showDemo = false;
};

struct Simulator {
    SimConfig cfg;
    std::mt19937 rng;

    int day = 0; 
    Warehouse wh;

    std::vector<ProductConfig> products;
    std::vector<std::string> shopNames;

    std::vector<SupplierDelivery> pipeline; 
    std::map<int, std::vector<Shipment>> shipmentsByDay; 
    std::vector<Order> todaysOrders;

    DayStats todaysStats{};
    DayStats totalStats{};
    std::vector<std::string> log;

    
    std::vector<Anim> dailyAnims;

    explicit Simulator(const SimConfig& c)
        : cfg(c), rng(c.seed), day(0), wh(c.K)
    {
        Reset(c);
    }

    void Reset(const SimConfig& c) {
        cfg = c;
        rng.seed(cfg.seed);
        day = 0;
        wh = Warehouse(cfg.K);
        products.clear();
        products.resize(cfg.K);
        pipeline.clear();
        shipmentsByDay.clear();
        todaysOrders.clear();
        todaysStats = {};
        totalStats = {};
        log.clear();
        dailyAnims.clear();

        shopNames.clear();
        for (int i = 0; i < cfg.M; ++i) {
            shopNames.push_back("Точка " + std::to_string(i+1));
        }

        
        std::vector<std::string> baseNames = {
            "Рис", "Макароны", "Мука", "Сахар", "Соль", "Масло", "Чай", "Кофе",
            "Тушёнка", "Фасоль", "Кукуруза", "Молоко", "Гречка", "Овсянка",
            "Печенье", "Консервы рыбные", "Сок", "Вода", "Специи", "Шоколад"
        };

        for (int i = 0; i < cfg.K; ++i) {
            ProductConfig pc;
            pc.name = (i < (int)baseNames.size()) ? baseNames[i] : ("Товар " + std::to_string(i+1));
            pc.unitName = "ед.";
            pc.unitsPerPackage = RandInt(rng, 6, 24);
            pc.capacityPackages = RandInt(rng, 18, 40);
            pc.shelfLifeDays = RandInt(rng, 7, 25);
            pc.discountBeforeDays = std::min(5, std::max(2, pc.shelfLifeDays / 5));
            pc.basePricePerUnit = RandFloat(rng, 0.8f, 5.5f);
            pc.discountPercent = 25.0f;
            pc.reorderThreshold = std::max(3, pc.capacityPackages / 5);
            pc.reorderAmount = std::max(4, pc.capacityPackages / 2);

            products[i] = pc;

            
            int initP = int(std::round(float(pc.capacityPackages) * RandFloat(rng, 0.30f, 0.70f)));
            wh.Receive(i, initP, products[i]);
        }

        PushLog("Симуляция сброшена. День = 0.");
    }

    void PushLog(const std::string& s) {
        log.push_back(s);
        if (log.size() > 400) log.erase(log.begin(), log.begin() + (log.size() - 400));
    }

    int IncomingPackages(int pid) const {
        int s = 0;
        for (auto& d : pipeline) if (d.productId == pid) s += d.packages;
        return s;
    }

    
    std::vector<int> PickDistinctProductsWeighted(int count) {
        count = std::min(count, cfg.K);
        std::vector<int> remaining(cfg.K);
        std::iota(remaining.begin(), remaining.end(), 0);

        std::vector<int> chosen;
        chosen.reserve(count);

        for (int pick = 0; pick < count; ++pick) {
            
            std::vector<float> w;
            w.reserve(remaining.size());
            float sum = 0.f;

            for (int pid : remaining) {
                float weight = 1.0f;
                if (wh.HasDiscountStock(pid)) {
                    weight += (products[pid].discountPercent / 100.0f) * cfg.discountDemandBoost;
                }
                w.push_back(weight);
                sum += weight;
            }
            if (sum <= 0.0001f) break;

            float r = RandFloat(rng, 0.f, sum);
            int idx = 0;
            for (; idx < (int)remaining.size(); ++idx) {
                r -= w[idx];
                if (r <= 0.f) break;
            }
            idx = std::clamp(idx, 0, (int)remaining.size()-1);

            chosen.push_back(remaining[idx]);
            remaining.erase(remaining.begin() + idx);
        }
        return chosen;
    }

    void CreateSupplierRequestIfNeeded(int pid) {
        const auto& pc = products[pid];
        int cur = wh.TotalPackages(pid);
        int incoming = IncomingPackages(pid);
        if (cur + incoming < pc.reorderThreshold) {
            int freeCap = pc.capacityPackages - (cur + incoming);
            int want = std::min(pc.reorderAmount, std::max(0, freeCap));
            if (want > 0) {
                SupplierDelivery d;
                d.productId = pid;
                d.packages = want;
                d.daysUntilArrival = RandInt(rng, cfg.supplierLeadTimeMin, cfg.supplierLeadTimeMax);
                pipeline.push_back(d);

                PushLog("Заявка поставщику: " + pc.name + " x" + std::to_string(want) +
                        " уп. (прибытие через " + std::to_string(d.daysUntilArrival) + " дн.)");
            }
        }
    }

    
    void BuildDailyAnimations(const std::vector<std::pair<int,int>>& supplierArrivals,
                              const std::vector<Order>& orders,
                              const std::vector<Shipment>& shipmentsDueToday,
                              const std::vector<std::pair<int,int>>& writeOffByProduct)
    {
        dailyAnims.clear();

        
        for (auto& [pid, packages] : supplierArrivals) {
            Anim a;
            a.type = AnimType::SupplyTruck;
            a.t0 = 0.02f;
            a.t1 = 0.22f;
            a.payload = packages;
            dailyAnims.push_back(a);
        }

        
        for (auto& o : orders) {
            Anim a;
            a.type = AnimType::OrderPaper;
            a.shopId = o.shopId;
            a.t0 = 0.10f + RandFloat(rng, 0.f, 0.08f);
            a.t1 = 0.42f + RandFloat(rng, 0.f, 0.06f);
            a.payload = (int)o.lines.size();
            dailyAnims.push_back(a);
        }

        
        int totalWriteOffPackages = 0;
        for (auto& [pid, pk] : writeOffByProduct) totalWriteOffPackages += pk;
        if (totalWriteOffPackages > 0) {
            Anim a;
            a.type = AnimType::WasteTruck;
            a.t0 = 0.30f;
            a.t1 = 0.55f;
            a.payload = totalWriteOffPackages;
            dailyAnims.push_back(a);
        }

        
        
        float base0 = 0.58f;
        float base1 = 0.98f;
        int idx = 0;
        for (auto& sh : shipmentsDueToday) {
            Anim a;
            a.type = AnimType::ShipmentTruck;
            a.shopId = sh.shopId;
            int totalPk = 0;
            for (auto& it : sh.items) totalPk += it.second;
            a.payload = totalPk;

            float span = (base1 - base0);
            float per = span / std::max(1, (int)shipmentsDueToday.size());
            a.t0 = base0 + per * idx + 0.01f;
            a.t1 = a.t0 + std::min(0.20f, per * 0.90f);
            idx++;
            dailyAnims.push_back(a);
        }
    }

    
    void AdvanceDay() {
        if (day >= cfg.N) return;

        day += 1;
        todaysOrders.clear();
        todaysStats = {};

        PushLog("--------------------------------------------------");
        PushLog("День " + std::to_string(day) + " начался.");

        
        std::vector<std::pair<int,int>> arrivals; 
        for (auto& d : pipeline) d.daysUntilArrival -= 1;

        {
            std::vector<SupplierDelivery> keep;
            keep.reserve(pipeline.size());
            for (auto& d : pipeline) {
                if (d.daysUntilArrival <= 0) {
                    arrivals.push_back({d.productId, d.packages});
                } else keep.push_back(d);
            }
            pipeline.swap(keep);
        }

        
        for (auto& [pid, pk] : arrivals) {
            int cur = wh.TotalPackages(pid);
            int freeCap = products[pid].capacityPackages - cur;
            int accepted = std::max(0, std::min(pk, freeCap));
            if (accepted > 0) {
                wh.Receive(pid, accepted, products[pid]);
                PushLog("Поставка: " + products[pid].name + " +" + std::to_string(accepted) + " уп.");
            }
            if (accepted < pk) {
                PushLog("Внимание: склад переполнен для " + products[pid].name +
                        " (не принято " + std::to_string(pk - accepted) + " уп.)");
            }
        }

        
        wh.DecrementShelfLife();

        
        std::vector<std::pair<int,int>> writeOffByProduct; 
        for (int pid = 0; pid < cfg.K; ++pid) {
            int pk = wh.WriteOffExpired(pid, products[pid], todaysStats);
            if (pk > 0) {
                writeOffByProduct.push_back({pid, pk});
                PushLog("Списание (просрочка): " + products[pid].name + " -" + std::to_string(pk) + " уп.");
            }
        }

        
        for (int pid = 0; pid < cfg.K; ++pid) {
            int aff = wh.ApplyDiscountIfNeeded(pid, products[pid]);
            if (aff > 0) {
                PushLog("Уценка: " + products[pid].name + " (" + std::to_string((int)products[pid].discountPercent) +
                        "%) затронуто " + std::to_string(aff) + " уп.");
            }
        }

        
        std::vector<Shipment> shipmentsDueToday;
        auto it = shipmentsByDay.find(day);
        if (it != shipmentsByDay.end()) shipmentsDueToday = it->second;

        
        for (int s = 0; s < cfg.M; ++s) {
            if (!RandBernoulli(rng, cfg.orderProbability)) continue;

            Order o;
            o.day = day;
            o.shopId = s;

            int maxLines = std::min(cfg.maxLinesPerOrder, cfg.K);
            int lines = RandInt(rng, 1, std::max(1, maxLines));

            auto picked = PickDistinctProductsWeighted(lines);
            for (int pid : picked) {
                OrderLine ln;
                ln.productId = pid;
                ln.requestedUnits = RandInt(rng, cfg.minUnitsPerLine, cfg.maxUnitsPerLine);
                o.lines.push_back(ln);
            }
            todaysOrders.push_back(o);
        }

        
        std::vector<Shipment> shipmentsTomorrow;
        for (auto& o : todaysOrders) {
            Shipment sh;
            sh.day = day + 1;
            sh.shopId = o.shopId;

            
            for (auto& ln : o.lines) {
                wh.FulfillLine(ln, products[ln.productId], rng, todaysStats);
                if (ln.deliveredPackages > 0) {
                    sh.items.push_back({ln.productId, ln.deliveredPackages});
                }
            }

            
            if (!sh.items.empty()) shipmentsTomorrow.push_back(sh);
        }

        if (!shipmentsTomorrow.empty()) {
            shipmentsByDay[day + 1].insert(shipmentsByDay[day + 1].end(),
                                           shipmentsTomorrow.begin(), shipmentsTomorrow.end());
            PushLog("Сформировано перевозок на завтра: " + std::to_string((int)shipmentsTomorrow.size()));
        } else {
            PushLog("На завтра перевозок не сформировано.");
        }

        
        for (int pid = 0; pid < cfg.K; ++pid) {
            CreateSupplierRequestIfNeeded(pid);
        }

        
        totalStats.revenue += todaysStats.revenue;
        totalStats.discountLoss += todaysStats.discountLoss;
        totalStats.writeOffLoss += todaysStats.writeOffLoss;
        totalStats.totalSoldUnits += todaysStats.totalSoldUnits;
        totalStats.totalWriteOffUnits += todaysStats.totalWriteOffUnits;

        PushLog("Итог дня " + std::to_string(day) + ": выручка=" + FmtMoney(todaysStats.revenue) +
                ", потери(уценка)=" + FmtMoney(todaysStats.discountLoss) +
                ", потери(списание)=" + FmtMoney(todaysStats.writeOffLoss));

        
        BuildDailyAnimations(arrivals, todaysOrders, shipmentsDueToday, writeOffByProduct);
    }
};




static void DrawSimulationCanvas(Simulator& sim, UIState& ui) {
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

    
    std::vector<ImVec2> shopPos(sim.cfg.M);
    ImVec2 shopsCenter = ImVec2(p0.x + sz.x * 0.75f, p0.y + sz.y * 0.50f);
    float rad = std::min(sz.x, sz.y) * 0.33f;

    for (int i = 0; i < sim.cfg.M; ++i) {
        float ang = (float)i / std::max(1, sim.cfg.M) * 2.0f * 3.1415926f - 0.6f;
        shopPos[i] = ImVec2(shopsCenter.x + std::cos(ang) * rad, shopsCenter.y + std::sin(ang) * rad);
        dl->AddCircleFilled(shopPos[i], R, IM_COL32(40, 70, 45, 255));
        dl->AddCircle(shopPos[i], R, IM_COL32(210, 255, 220, 255), 0, 2.f);
        dl->AddText(ImVec2(shopPos[i].x - R, shopPos[i].y + R + 2), IM_COL32(230, 230, 240, 255), sim.shopNames[i].c_str());

        
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

        std::string t = "День " + std::to_string(sim.day) + "/" + std::to_string(sim.cfg.N) +
                        "   (" + std::to_string((int)std::round(ui.dayProgress*100)) + "%)";
        dl->AddText(ImVec2(bar0.x + 8, bar0.y + 2), IM_COL32(240, 240, 250, 255), t.c_str());
    }

    
    auto iconRect = [&](ImVec2 c, float s)->std::pair<ImVec2,ImVec2>{
        return {ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s)};
    };

    for (auto& a : sim.dailyAnims) {
        if (ui.dayProgress < a.t0 || ui.dayProgress > a.t1) continue;
        float t = (ui.dayProgress - a.t0) / std::max(0.0001f, (a.t1 - a.t0));

        ImVec2 start = warehouseC, end = warehouseC;
        ImU32 col = IM_COL32(255,255,255,255);

        if (a.type == AnimType::SupplyTruck) {
            start = supplierC;
            end = warehouseC;
            col = IM_COL32(130, 200, 255, 255);
        } else if (a.type == AnimType::WasteTruck) {
            start = warehouseC;
            end = dumpC;
            col = IM_COL32(255, 160, 160, 255);
        } else if (a.type == AnimType::OrderPaper) {
            start = shopPos[a.shopId];
            end = warehouseC;
            col = IM_COL32(255, 235, 150, 255);
        } else if (a.type == AnimType::ShipmentTruck) {
            start = warehouseC;
            end = shopPos[a.shopId];
            col = IM_COL32(170, 255, 190, 255);
        }

        ImVec2 pos = Lerp(start, end, t);

        
        dl->AddLine(start, pos, IM_COL32(255,255,255,45), 3.f);

        
        float s = R * 0.60f;
        auto [r0, r1] = iconRect(pos, s);

        if (a.type == AnimType::OrderPaper) {
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

static void ProductEditor(Simulator& sim) {
    if (ImGui::BeginTable("products", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                          ImVec2(0, 240))) {
        ImGui::TableSetupColumn("Товар");
        ImGui::TableSetupColumn("Упак(ед.)");
        ImGui::TableSetupColumn("Ёмк(уп.)");
        ImGui::TableSetupColumn("Срок(дн.)");
        ImGui::TableSetupColumn("Уценка до(дн.)");
        ImGui::TableSetupColumn("Цена/ед");
        ImGui::TableSetupColumn("Уценка %");
        ImGui::TableSetupColumn("Остаток(уп.)");
        ImGui::TableHeadersRow();

        for (int pid = 0; pid < sim.cfg.K; ++pid) {
            auto& p = sim.products[pid];
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
            ImGui::Text("%d", sim.wh.TotalPackages(pid));
        }
        ImGui::EndTable();
    }

    
    for (auto& p : sim.products) {
        p.unitsPerPackage = std::clamp(p.unitsPerPackage, 1, 1000);
        p.capacityPackages = std::clamp(p.capacityPackages, 0, 5000);
        p.shelfLifeDays = std::clamp(p.shelfLifeDays, 1, 365);
        p.discountBeforeDays = std::clamp(p.discountBeforeDays, 0, 365);
        p.basePricePerUnit = std::max(0.01f, p.basePricePerUnit);
        p.discountPercent = std::clamp(p.discountPercent, 0.f, 95.f);
        p.reorderThreshold = std::clamp(p.reorderThreshold, 0, p.capacityPackages);
        p.reorderAmount = std::clamp(p.reorderAmount, 0, p.capacityPackages);
    }
}

static void OrdersTable(const std::vector<Order>& orders, const std::vector<ProductConfig>& products) {
    if (ImGui::BeginTable("orders", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                          ImVec2(0, 220))) {
        ImGui::TableSetupColumn("Точка");
        ImGui::TableSetupColumn("Товар");
        ImGui::TableSetupColumn("Запрос(ед)");
        ImGui::TableSetupColumn("Выбрано(уп)");
        ImGui::TableSetupColumn("Выдано(уп)");
        ImGui::TableSetupColumn("Выдано(ед)");
        ImGui::TableHeadersRow();

        for (auto& o : orders) {
            for (auto& ln : o.lines) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%d", o.shopId + 1);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(products[ln.productId].name.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%d", ln.requestedUnits);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%d", ln.chosenPackages);
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%d", ln.deliveredPackages);
                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%d", ln.deliveredUnits);
            }
        }
        ImGui::EndTable();
    }
}

static void LogView(const std::vector<std::string>& log) {
    ImGui::BeginChild("log", ImVec2(0, 180), true);
    int start = (int)log.size() > 200 ? (int)log.size() - 200 : 0;
    for (int i = start; i < (int)log.size(); ++i) {
        ImGui::TextUnformatted(log[i].c_str());
    }
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
    if (!font) {
        
        io.Fonts->AddFontDefault();
    }

    ImGui::StyleColorsDark();

    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL2_Init();

    
    ImGui_ImplOpenGL2_CreateFontsTexture();

    
    SimConfig cfg;
    cfg.N = 20; cfg.M = 5; cfg.K = 12;
    cfg.seed = 12345;
    Simulator sim(cfg);
    UIState ui;

    bool done = false;
    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) done = true;
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE &&
                event.window.windowID == SDL_GetWindowID(window)) done = true;
        }

        
        if (ui.running && sim.day < sim.cfg.N) {
            float secondsPerDay = std::max(0.25f, sim.cfg.secondsPerDay / std::max(0.1f, ui.speed));
            ui.dayProgress += (io.DeltaTime / secondsPerDay);
            if (ui.dayProgress >= 1.f) {
                ui.dayProgress = 0.f;
                sim.AdvanceDay();
                if (sim.day >= sim.cfg.N) ui.running = false;
            }
        }

        
        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        
        ImGui::Begin("Управление симуляцией");

        ImGui::Text("Дата: день %d из %d", sim.day, sim.cfg.N);
        ImGui::Separator();

        
        if (ImGui::Button(ui.running ? "Пауза" : "Старт")) {
            if (sim.day == 0 && !ui.running) {
                
                sim.AdvanceDay();
            }
            ui.running = !ui.running;
        }
        ImGui::SameLine();
        if (ImGui::Button("Шаг (1 день)")) {
            ui.running = false;
            if (sim.day < sim.cfg.N) {
                ui.dayProgress = 0.f;
                sim.AdvanceDay();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Сброс")) {
            ui.running = false;
            ui.dayProgress = 0.f;
            sim.Reset(sim.cfg);
        }

        ImGui::SameLine();
        ImGui::Checkbox("ImGui Demo", &ui.showDemo);

        ImGui::SliderFloat("Скорость", &ui.speed, 0.25f, 4.0f, "%.2fx");
        ImGui::SliderFloat("Секунд на день", &sim.cfg.secondsPerDay, 1.5f, 14.0f, "%.1f");

        ImGui::Separator();
        ImGui::Text("Параметры моделирования (можно менять, затем 'Сброс')");

        ImGui::InputInt("N (дней)", &sim.cfg.N);
        ImGui::InputInt("M (точек)", &sim.cfg.M);
        ImGui::InputInt("K (товаров)", &sim.cfg.K);
        ImGui::InputScalar("Seed", ImGuiDataType_U32, &sim.cfg.seed);

        sim.cfg.N = std::clamp(sim.cfg.N, 10, 30);
        sim.cfg.M = std::clamp(sim.cfg.M, 3, 9);
        sim.cfg.K = std::clamp(sim.cfg.K, 12, 20);

        ImGui::SliderFloat("P(заказ в день от точки)", &sim.cfg.orderProbability, 0.0f, 1.0f, "%.2f");
        ImGui::InputInt("Max позиций в заказе", &sim.cfg.maxLinesPerOrder);
        ImGui::InputInt("Мин ед. в строке", &sim.cfg.minUnitsPerLine);
        ImGui::InputInt("Макс ед. в строке", &sim.cfg.maxUnitsPerLine);
        ImGui::SliderFloat("Усиление спроса от уценки", &sim.cfg.discountDemandBoost, 0.0f, 5.0f, "%.2f");

        sim.cfg.maxLinesPerOrder = std::clamp(sim.cfg.maxLinesPerOrder, 1, 20);
        sim.cfg.minUnitsPerLine = std::max(1, sim.cfg.minUnitsPerLine);
        sim.cfg.maxUnitsPerLine = std::max(sim.cfg.minUnitsPerLine, sim.cfg.maxUnitsPerLine);

        ImGui::Separator();
        ImGui::Text("Итоги сегодня:");
        ImGui::BulletText("Выручка: %s", FmtMoney(sim.todaysStats.revenue).c_str());
        ImGui::BulletText("Потери (уценка): %s", FmtMoney(sim.todaysStats.discountLoss).c_str());
        ImGui::BulletText("Потери (списание): %s", FmtMoney(sim.todaysStats.writeOffLoss).c_str());
        ImGui::BulletText("Продано (ед.): %d", sim.todaysStats.totalSoldUnits);
        ImGui::BulletText("Списано (ед.): %d", sim.todaysStats.totalWriteOffUnits);

        ImGui::Text("Итоги за период:");
        ImGui::BulletText("Выручка: %s", FmtMoney(sim.totalStats.revenue).c_str());
        ImGui::BulletText("Потери (уценка): %s", FmtMoney(sim.totalStats.discountLoss).c_str());
        ImGui::BulletText("Потери (списание): %s", FmtMoney(sim.totalStats.writeOffLoss).c_str());
        ImGui::BulletText("Продано (ед.): %d", sim.totalStats.totalSoldUnits);
        ImGui::BulletText("Списано (ед.): %d", sim.totalStats.totalWriteOffUnits);

        ImGui::End();

        
        ImGui::Begin("Склад: товары/настройка уценки (заведующий)");
        ProductEditor(sim);
        ImGui::End();

        ImGui::Begin("Заказы текущего дня (с обработкой упаковками)");
        OrdersTable(sim.todaysOrders, sim.products);
        ImGui::End();

        ImGui::Begin("Лог / события");
        LogView(sim.log);
        ImGui::End();

        ImGui::Begin("Визуализация (движение процессов)");
        DrawSimulationCanvas(sim, ui);
        ImGui::End();

        if (ui.showDemo) ImGui::ShowDemoWindow(&ui.showDemo);

        
        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.08f, 0.09f, 0.10f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    
    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
