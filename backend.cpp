// backend.cpp
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <map>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace ws {

// -----------------------------
// Public POD structs (используются и в frontend.cpp)
// -----------------------------
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

// -----------------------------
// Helpers
// -----------------------------
static float Clamp01(float x) { return (x < 0.f) ? 0.f : (x > 1.f ? 1.f : x); }

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

static std::string FmtMoney(double v) {
    std::ostringstream ss;
    ss.setf(std::ios::fixed);
    ss << std::setprecision(2) << v;
    return ss.str();
}

static void ClampConfig(SimConfig& c) {
    c.N = std::clamp(c.N, 10, 30);
    c.M = std::clamp(c.M, 3, 9);
    c.K = std::clamp(c.K, 12, 20);

    c.orderProbability = std::clamp(c.orderProbability, 0.0f, 1.0f);
    c.maxLinesPerOrder = std::clamp(c.maxLinesPerOrder, 1, 20);
    c.minUnitsPerLine = std::max(1, c.minUnitsPerLine);
    c.maxUnitsPerLine = std::max(c.minUnitsPerLine, c.maxUnitsPerLine);
    c.discountDemandBoost = std::clamp(c.discountDemandBoost, 0.0f, 10.0f);

    c.supplierLeadTimeMin = std::clamp(c.supplierLeadTimeMin, 1, 30);
    c.supplierLeadTimeMax = std::clamp(c.supplierLeadTimeMax, c.supplierLeadTimeMin, 60);

    c.secondsPerDay = std::clamp(c.secondsPerDay, 0.25f, 30.0f);
}

static void ClampProduct(ProductConfig& p) {
    p.unitsPerPackage = std::clamp(p.unitsPerPackage, 1, 1000);
    p.capacityPackages = std::clamp(p.capacityPackages, 0, 5000);
    p.shelfLifeDays = std::clamp(p.shelfLifeDays, 1, 365);
    p.discountBeforeDays = std::clamp(p.discountBeforeDays, 0, p.shelfLifeDays);
    p.basePricePerUnit = std::max(0.01f, p.basePricePerUnit);
    p.discountPercent = std::clamp(p.discountPercent, 0.f, 95.f);
    p.reorderThreshold = std::clamp(p.reorderThreshold, 0, p.capacityPackages);
    p.reorderAmount = std::clamp(p.reorderAmount, 0, p.capacityPackages);
}

// -----------------------------
// Internal simulation model
// -----------------------------
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
    std::vector<std::pair<int,int>> items; // pid, packages
};

struct Warehouse {
    std::vector<std::deque<Batch>> inv;

    explicit Warehouse(int K=0) { inv.resize(K); }

    int TotalPackages(int pid) const {
        if (pid < 0 || pid >= (int)inv.size()) return 0;
        int s = 0;
        for (auto& b : inv[pid]) s += b.packages;
        return s;
    }

    bool HasDiscountStock(int pid) const {
        if (pid < 0 || pid >= (int)inv.size()) return false;
        for (auto& b : inv[pid]) if (b.packages > 0 && b.discounted) return true;
        return false;
    }

    void DecrementShelfLife() {
        for (auto& dq : inv)
            for (auto& b : dq)
                b.daysLeft -= 1;
    }

    int WriteOffExpired(int pid, const ProductConfig& pc, DayStats& st) {
        if (pid < 0 || pid >= (int)inv.size()) return 0;
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
        if (pid < 0 || pid >= (int)inv.size()) return;
        if (packages <= 0) return;
        Batch b;
        b.packages = packages;
        b.daysLeft = pc.shelfLifeDays;
        b.discounted = false;
        b.unitPrice = pc.basePricePerUnit;
        inv[pid].push_back(b);
    }

    int ApplyDiscountIfNeeded(int pid, const ProductConfig& pc) {
        if (pid < 0 || pid >= (int)inv.size()) return 0;
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

    void RepriceBatches(int pid, const ProductConfig& pc) {
        if (pid < 0 || pid >= (int)inv.size()) return;
        for (auto& b : inv[pid]) {
            if (b.discounted)
                b.unitPrice = pc.basePricePerUnit * (1.0f - pc.discountPercent / 100.0f);
            else
                b.unitPrice = pc.basePricePerUnit;
        }
    }

    void FulfillLine(OrderLine& line,
                     const ProductConfig& pc,
                     std::mt19937& rng,
                     DayStats& st)
    {
        if (line.requestedUnits <= 0) return;
        if (pc.unitsPerPackage <= 0) return;

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

            if (b.discounted) st.discountLoss += (base - actual);

            if (b.packages == 0) dq.pop_front();
        }
    }
};

// -----------------------------
// Simulator implementation
// -----------------------------
struct SimulatorImpl {
    SimConfig cfg{};
    std::mt19937 rng{12345};

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

    explicit SimulatorImpl(const SimConfig& c)
        : cfg(c), rng(c.seed), day(0), wh(c.K)
    {
        Reset(c);
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

    void Reset(const SimConfig& c) {
        cfg = c;
        ClampConfig(cfg);
        rng.seed(cfg.seed);
        day = 0;

        wh = Warehouse(cfg.K);
        products.assign(cfg.K, {});
        pipeline.clear();
        shipmentsByDay.clear();
        todaysOrders.clear();
        todaysStats = {};
        totalStats = {};
        log.clear();
        dailyAnims.clear();

        shopNames.clear();
        for (int i = 0; i < cfg.M; ++i)
            shopNames.push_back("Точка " + std::to_string(i+1));

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

            ClampProduct(pc);
            products[i] = pc;

            int initP = int(std::round(float(pc.capacityPackages) * RandFloat(rng, 0.30f, 0.70f)));
            wh.Receive(i, initP, products[i]);
        }

        PushLog("Симуляция сброшена. День = 0.");
    }

    void ClampAllProducts() {
        for (int pid = 0; pid < (int)products.size(); ++pid) {
            ClampProduct(products[pid]);
            wh.RepriceBatches(pid, products[pid]);
        }
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
            (void)pid;
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
        for (auto& [pid, pk] : writeOffByProduct) { (void)pid; totalWriteOffPackages += pk; }
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

        // важная страховка: всегда держим продукты валидными
        ClampAllProducts();

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
                if (d.daysUntilArrival <= 0) arrivals.push_back({d.productId, d.packages});
                else keep.push_back(d);
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
                if (ln.deliveredPackages > 0)
                    sh.items.push_back({ln.productId, ln.deliveredPackages});
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

        for (int pid = 0; pid < cfg.K; ++pid) CreateSupplierRequestIfNeeded(pid);

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

// -----------------------------
// Opaque handle + API
// -----------------------------
struct SimulatorHandle {
    SimulatorImpl sim;
    explicit SimulatorHandle(const SimConfig& c) : sim(c) {}
};

SimulatorHandle* CreateSimulator(const SimConfig& cfg) {
    SimConfig c = cfg;
    ClampConfig(c);
    return new SimulatorHandle(c);
}

void DestroySimulator(SimulatorHandle* h) { delete h; }

void ResetSimulator(SimulatorHandle* h, const SimConfig& cfg) {
    if (!h) return;
    SimConfig c = cfg;
    ClampConfig(c);
    h->sim.Reset(c);
}

void AdvanceDay(SimulatorHandle* h) {
    if (!h) return;
    h->sim.AdvanceDay();
}

int GetDay(const SimulatorHandle* h) { return h ? h->sim.day : 0; }

SimConfig GetConfigCopy(const SimulatorHandle* h) { return h ? h->sim.cfg : SimConfig{}; }

void SetRuntimeTuning(SimulatorHandle* h,
                      float orderProbability,
                      int maxLinesPerOrder,
                      int minUnitsPerLine,
                      int maxUnitsPerLine,
                      float discountDemandBoost,
                      float secondsPerDay)
{
    if (!h) return;
    h->sim.cfg.orderProbability = orderProbability;
    h->sim.cfg.maxLinesPerOrder = maxLinesPerOrder;
    h->sim.cfg.minUnitsPerLine = minUnitsPerLine;
    h->sim.cfg.maxUnitsPerLine = maxUnitsPerLine;
    h->sim.cfg.discountDemandBoost = discountDemandBoost;
    h->sim.cfg.secondsPerDay = secondsPerDay;
    ClampConfig(h->sim.cfg);
}

DayStats GetTodaysStats(const SimulatorHandle* h) { return h ? h->sim.todaysStats : DayStats{}; }
DayStats GetTotalStats(const SimulatorHandle* h) { return h ? h->sim.totalStats : DayStats{}; }

void GetShopNames(const SimulatorHandle* h, std::vector<std::string>& out) {
    out.clear();
    if (!h) return;
    out = h->sim.shopNames;
}

void GetProductConfigs(const SimulatorHandle* h, std::vector<ProductConfig>& out) {
    out.clear();
    if (!h) return;
    out = h->sim.products;
}

void SetProductConfigs(SimulatorHandle* h, const std::vector<ProductConfig>& in) {
    if (!h) return;
    if ((int)in.size() != h->sim.cfg.K) return; // K менять только через Reset
    h->sim.products = in;
    h->sim.ClampAllProducts();
}

int GetStockPackages(const SimulatorHandle* h, int pid) {
    if (!h) return 0;
    return h->sim.wh.TotalPackages(pid);
}

bool HasDiscountStock(const SimulatorHandle* h, int pid) {
    if (!h) return false;
    return h->sim.wh.HasDiscountStock(pid);
}

void GetTodaysOrderRows(const SimulatorHandle* h, std::vector<OrderRow>& out) {
    out.clear();
    if (!h) return;

    for (auto& o : h->sim.todaysOrders) {
        for (auto& ln : o.lines) {
            OrderRow r;
            r.shopId = o.shopId;
            r.productId = ln.productId;
            r.requestedUnits = ln.requestedUnits;
            r.chosenPackages = ln.chosenPackages;
            r.deliveredPackages = ln.deliveredPackages;
            r.deliveredUnits = ln.deliveredUnits;
            out.push_back(r);
        }
    }
}

void GetLog(const SimulatorHandle* h, std::vector<std::string>& out, int maxLines) {
    out.clear();
    if (!h) return;
    maxLines = std::max(1, maxLines);
    int start = (int)h->sim.log.size() > maxLines ? (int)h->sim.log.size() - maxLines : 0;
    for (int i = start; i < (int)h->sim.log.size(); ++i) out.push_back(h->sim.log[i]);
}

void GetDailyAnims(const SimulatorHandle* h, std::vector<Anim>& out) {
    out.clear();
    if (!h) return;
    out = h->sim.dailyAnims;
}

} // namespace ws
