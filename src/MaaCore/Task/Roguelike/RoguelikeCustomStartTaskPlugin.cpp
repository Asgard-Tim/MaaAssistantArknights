#include <unordered_set>
#include "RoguelikeCustomStartTaskPlugin.h"

#include "Config/GeneralConfig.h"
#include "Config/Miscellaneous/BattleDataConfig.h"
#include "Config/TaskData.h"
#include "Controller/Controller.h"
#include "Task/ProcessTask.h"
#include "Utils/Logger.hpp"
#include "Vision/Miscellaneous/PipelineAnalyzer.h"
#include "Vision/OCRer.h"
#include "Vision/MultiMatcher.h"

bool asst::RoguelikeCustomStartTaskPlugin::verify(AsstMsg msg, const json::value& details) const
{
    if (details.get("subtask", std::string()) != "ProcessTask") {
        return false;
    }

    if (!RoguelikeConfig::is_valid_theme(m_config->get_theme())) {
        Log.error("Roguelike name doesn't exist!");
        return false;
    }

    const std::string roguelike_name = m_config->get_theme() + "@";
    const std::string& task = details.get("details", "task", "");
    std::string_view task_view = task;
    if (task_view.starts_with(roguelike_name)) {
        task_view.remove_prefix(roguelike_name.length());
    }
    static const std::array<std::tuple<AsstMsg, std::string_view, RoguelikeCustomType>, 6> TaskMap = {
        std::make_tuple(AsstMsg::SubTaskCompleted, "Roguelike@Squad-EnterPoint", RoguelikeCustomType::Squad),
        std::make_tuple(AsstMsg::SubTaskStart, "Roguelike@LastReward-EnterPoint", RoguelikeCustomType::Reward),
        std::make_tuple(AsstMsg::SubTaskCompleted, "Roguelike@RolesDefault", RoguelikeCustomType::Roles),
        std::make_tuple(AsstMsg::SubTaskStart, "Roguelike@RecruitMain", RoguelikeCustomType::CoreChar),
        std::make_tuple(AsstMsg::SubTaskStart, "Roguelike@Stages", RoguelikeCustomType::FirstFloorNodes),
        std::make_tuple(AsstMsg::SubTaskStart, "Roguelike@ExitThenAbandon", RoguelikeCustomType::RestoreRetry),
    };

    m_waiting_to_run = RoguelikeCustomType::None;
    for (const auto& [t_msg, t_task, t] : TaskMap) {
        if (t_msg == msg && task_view.ends_with(t_task)) {
            m_waiting_to_run = t;
            break;
        }
    }

    if (m_waiting_to_run == RoguelikeCustomType::None) {
        return false;
    }
    if (m_waiting_to_run == RoguelikeCustomType::Reward) {
        return true;
    }
    if (m_waiting_to_run == RoguelikeCustomType::Squad) {
        if (m_config->get_run_for_collectible()) { // 烧水分队
        }
        else {                                     // 开局分队
        }
        return true;
    }
    if (m_waiting_to_run == RoguelikeCustomType::CoreChar) {
        return !m_config->get_core_char().empty();
    }
    if (m_waiting_to_run == RoguelikeCustomType::FirstFloorNodes) {
        return m_need_check_first_floor;
    }
    if (m_waiting_to_run == RoguelikeCustomType::RestoreRetry) {
        return m_need_restore_retry;
    }

    // Roles CoreChar
    if (auto it = m_customs.find(m_waiting_to_run); it == m_customs.cend()) {
        return false;
    }
    else if (it->second.empty()) {
        return false;
    }

    return true;
}

bool asst::RoguelikeCustomStartTaskPlugin::load_params(const json::value& params)
{
    m_squad = params.get("squad", "");
    if (m_config->get_mode() == RoguelikeMode::Collectible) {
        m_collectible_mode_squad = params.get("collectible_mode_squad", m_squad);
    }

    m_config->set_core_char(params.get("core_char", ""));                            // 开局干员名
    set_custom(RoguelikeCustomType::Roles, params.get("roles", ""));                 // 开局职业组
    m_config->set_use_support(params.get("use_support", false));                     // 开局干员是否为助战干员
    m_config->set_use_nonfriend_support(params.get("use_nonfriend_support", false)); // 是否可以是非好友助战干员

    if (auto select_list = params.find<json::object>("collectible_mode_start_list"); select_list) {
        RoguelikeStartSelect list;
        list.hot_water = select_list->get("hot_water", false);
        list.shield = select_list->get("shield", false);
        list.ingot = select_list->get("ingot", false);
        list.hope = select_list->get("hope", false);
        list.random = select_list->get("random", false);
        if (m_config->get_theme() == RoguelikeTheme::Mizuki) {
            list.key = select_list->get("key", false);
            list.dice = select_list->get("dice", false);
        }
        else if (m_config->get_theme() == RoguelikeTheme::Sarkaz) {
            list.ideas = select_list->get("ideas", false);
        }
        else if (m_config->get_theme() == RoguelikeTheme::JieGarden) {
            list.ticket = select_list->get("ticket", false);
        }
        m_start_select = list;
    }

    return true;
}

void asst::RoguelikeCustomStartTaskPlugin::set_custom(RoguelikeCustomType type, std::string custom)
{
    m_customs.insert_or_assign(type, std::move(custom));
}

bool asst::RoguelikeCustomStartTaskPlugin::_run()
{
    const std::unordered_map<RoguelikeCustomType, std::function<bool(void)>> TypeActuator = {
        { RoguelikeCustomType::Squad, std::bind(&RoguelikeCustomStartTaskPlugin::hijack_squad, this) },
        { RoguelikeCustomType::Reward, std::bind(&RoguelikeCustomStartTaskPlugin::hijack_reward, this) },
        { RoguelikeCustomType::Roles, std::bind(&RoguelikeCustomStartTaskPlugin::hijack_roles, this) },
        { RoguelikeCustomType::CoreChar, std::bind(&RoguelikeCustomStartTaskPlugin::hijack_core_char, this) },
        { RoguelikeCustomType::FirstFloorNodes, std::bind(&RoguelikeCustomStartTaskPlugin::hijack_first_floor_nodes, this) },
        { RoguelikeCustomType::RestoreRetry, std::bind(&RoguelikeCustomStartTaskPlugin::restore_retry, this) },
    };

    auto it = TypeActuator.find(m_waiting_to_run);
    if (it == TypeActuator.cend()) {
        return false;
    }

    return it->second();
}

bool asst::RoguelikeCustomStartTaskPlugin::hijack_squad()
{
    std::string squad = !m_config->get_run_for_collectible() ? m_squad : m_collectible_mode_squad;
    if (squad.empty()) { // 简单处理，认为指挥分队无需滑屏，没有就随机
        return ProcessTask(
                   *this,
                   { m_config->get_theme() + "@Roguelike@SquadDefault",
                     m_config->get_theme() + "@Roguelike@Squad-Random" })
            .run();
    }

    constexpr size_t SwipeTimes = 7;
    for (size_t i = 0; i != SwipeTimes; ++i) {
        if (need_exit()) {
            return false;
        }
        auto image = ctrler()->get_image();
        OCRer analyzer(image);
        analyzer.set_task_info("RoguelikeCustom-HijackSquad");
        analyzer.set_required({ squad });

        if (!analyzer.analyze()) {
            ProcessTask(*this, { "Roguelike@SquadSlowlySwipeToTheRight" }).run();
            sleep(Task.get("RoguelikeCustom-HijackSquad")->post_delay);
            continue;
        }
        const auto& rect = analyzer.get_result().front().rect;
        ctrler()->click(rect);

        m_config->set_squad(std::move(squad));
        return true;
    }
    ProcessTask(*this, { "SwipeToTheLeft" }).run();
    return false;
}

bool asst::RoguelikeCustomStartTaskPlugin::hijack_reward()
{
    const auto& list = get_select_list();
    if (list.empty()) {
        // 执行默认选择顺序
        ProcessTask(*this, { m_config->get_theme() + "@Roguelike@LastReward-Strategy" }).run();
        return true;
    }

    // 处理选择顺序
    PipelineAnalyzer analyzer(ctrler()->get_image());
    analyzer.set_tasks(list);
    if (auto ret = analyzer.analyze(); !ret) {
        // 未获取到期望物品，设置烧水flag，重开
        m_config->set_run_for_collectible(true);
        m_control_ptr->exit_then_stop(true);
    }
    else if (m_config->get_start_with_elite_two() || m_config->get_first_floor_foldartal()) {
        // 之后还要凹开局精二或第一层密文板，不停止任务，继续探索
        ctrler()->click(ret->rect);
        sleep(Config.get_options().task_delay);
    }
    else {
        m_control_ptr->exit_then_stop(false);
        m_task_ptr->set_enable(false);
    }

    return true;
}

bool asst::RoguelikeCustomStartTaskPlugin::hijack_roles()
{
    constexpr size_t SwipeTimes = 7;
    const std::string& required_role = m_customs[RoguelikeCustomType::Roles];

    for (size_t i = 0; i != SwipeTimes; ++i) {
        if (need_exit()) {
            return false;
        }

        auto image = ctrler()->get_image();
        OCRer analyzer(image);
        analyzer.set_task_info("RoguelikeCustom-HijackRoles");
        analyzer.set_required({ required_role });

        if (analyzer.analyze()) {
            const auto& rect = analyzer.get_result().front().rect;
            ctrler()->click(rect);
            return true;
        }

        ProcessTask(*this, { "Roguelike@SquadSlowlySwipeToTheRight" }).run();
        sleep(Task.get("RoguelikeCustom-HijackRoles")->post_delay);
    }

    ProcessTask(*this, { "SwipeToTheLeft" }).run();
    return false;
}

bool asst::RoguelikeCustomStartTaskPlugin::hijack_core_char()
{
    static const std::unordered_map<battle::Role, std::string> RoleOcrNameMap = {
        { battle::Role::Caster, "术师" }, { battle::Role::Medic, "医疗" },   { battle::Role::Pioneer, "先锋" },
        { battle::Role::Sniper, "狙击" }, { battle::Role::Special, "特种" }, { battle::Role::Support, "辅助" },
        { battle::Role::Tank, "重装" },   { battle::Role::Warrior, "近卫" }
    };
    const std::string& char_name = m_config->get_core_char();
    const auto& role = BattleData.get_role(char_name);
    auto role_iter = RoleOcrNameMap.find(role);
    if (role_iter == RoleOcrNameMap.cend()) {
        Log.error("Unknown role", char_name, static_cast<int>(role));
        return false;
    }
    // select role
    const std::string& role_ocr_name = role_iter->second;
    Log.info("role", role_ocr_name);
    auto image = ctrler()->get_image();
    OCRer analyzer(image);
    analyzer.set_task_info("RoguelikeCustom-HijackCoChar");
    analyzer.set_required({ role_ocr_name });
    if (!analyzer.analyze()) {
        return false;
    }
    for (int retry = 0; retry < 3; ++retry) {
        const auto& role_rect = analyzer.get_result().front().rect;
        ctrler()->click(role_rect);
        sleep(Task.get("RoguelikeCustom-HijackCoChar")->pre_delay);

        ProcessTask check(
            *this,
            { m_config->get_theme() + "@Roguelike@ChooseOperFlag",
              m_config->get_theme() + "@Roguelike@RecruitCloseGuide" });
        check.set_times_limit("Roguelike@ChooseOperFlag", 0);
        check.set_retry_times(0);
        if (check.run()) {
            return true; // 进入选择干员界面
        }
    }
    return false; // 进入选择干员界面失败
}


bool asst::RoguelikeCustomStartTaskPlugin::restore_retry()
{
    Log.info("Restoring retry_times to default (20) for subsequent pipeline tasks");
    m_need_restore_retry = false;
    m_task_ptr->set_retry_times(RetryTimesDefault);
    return true;
}

void asst::RoguelikeCustomStartTaskPlugin::reset_in_run_variables()
{
    m_need_check_first_floor = m_config->get_check_regional_commissions();
    m_need_restore_retry = false;
}

bool asst::RoguelikeCustomStartTaskPlugin::hijack_first_floor_nodes()
{
    LogTraceFunction;

    m_need_check_first_floor = false;

    const std::string& theme = m_config->get_theme();

    // Map template names to Chinese display names
    static const std::unordered_map<std::string, std::string> NodeNameMap = {
        { "Mizuki@Roguelike@StageRegionalCommissioning", "地区委托" },
        { "Mizuki@Roguelike@StageCombatOps", "作战" },
        { "Mizuki@Roguelike@StageEmergencyOps", "紧急作战" },
        { "Mizuki@Roguelike@StageEncounter", "不期而遇" },
        { "Mizuki@Roguelike@StageTrader", "诡意行商" },
        { "Mizuki@Roguelike@StageSafeHouse", "安全屋" },
        { "Mizuki@Roguelike@StageBoons", "得偿所愿" },
        { "Mizuki@Roguelike@StageDreadfulFoe", "险路恶敌" },
        { "Mizuki@Roguelike@StageDreadfulFoe-5", "险路恶敌" },
        { "Mizuki@Roguelike@StageGambling", "赌局" },
        { "Mizuki@Roguelike@StageWindAndRain", "失与得" },
        { "Mizuki@Roguelike@StageEmergencyTransportation", "先行一步" },
    };

    bool found_target = false;
    json::array nodes_display;
    std::unordered_set<std::string> seen_names; // dedup across columns

    // After entering floor 1, view starts at leftmost (start node position).
    // Swipe right column-by-column to scan all nodes on floor 1 (up to 3 node columns + boss).
    Log.info("Starting floor 1 node scan");

    auto image = ctrler()->get_image();
    MultiMatcher analyzer(image);
    analyzer.set_task_info(theme + "@RoguelikeRoutingNodeAnalyze-RegionalCommissions");

    // Scan up to 5 columns (first floor has up to 3-4 columns + boss, with some overlap)
    int empty_cols = 0;
    for (int col = 0; col < 5 && !found_target && empty_cols < 2; ++col) {
        if (col > 0) {
            ProcessTask(*this, { "RoguelikeRouting-MoveRight" }).run();
            sleep(200);
            image = ctrler()->get_image();
            analyzer.set_image(image);
        }

        auto results_opt = analyzer.analyze();
        int col_hits = 0;
        if (results_opt) {
            for (const auto& [rect, score, templ_name] : *results_opt) {
                std::string name_no_png = templ_name;
                if (name_no_png.ends_with(".png")) {
                    name_no_png.resize(name_no_png.size() - 4);
                }
                auto it = NodeNameMap.find(name_no_png);
                std::string display_name = it != NodeNameMap.end() ? it->second : name_no_png;

                // Dedup by display name
                if (seen_names.insert(display_name).second) {
                    nodes_display.emplace_back(display_name);
                    ++col_hits;
                }

                if (display_name == "地区委托") {
                    found_target = true;
                    break;
                }
            }
        }

        Log.info("Column", col, "scan:", col_hits, "new nodes found, total unique:", seen_names.size());

        if (col_hits == 0) {
            ++empty_cols;
        }
        else {
            empty_cols = 0;
        }
    }

    // Report results to GUI info bar (human-readable Chinese)
    std::string summary = "第一层节点: ";
    if (nodes_display.empty()) {
        summary += "未识别到任何节点";
    }
    else {
        for (size_t i = 0; i < nodes_display.size(); ++i) {
            if (i > 0) summary += "、";
            summary += nodes_display[i].as_string();
        }
    }
    json::value info = json::object {
        { "what", "FirstFloorNodeCheck" },
        { "details",
          json::object {
              { "summary", summary },
              { "found_target", found_target },
          } },
    };
    callback(AsstMsg::ConnectionInfo, info);

    if (found_target) {
        Log.info("RegionalCommissions node FOUND, stopping task immediately (no exit/return).");
        m_task_ptr->set_enable(false);
    }
    else {
        Log.info("RegionalCommissions node NOT found, setting retry_times=0 to force onErrorNext -> ExitThenAbandon");
        // Stages has onErrorNext: ["Mizuki@Roguelike@ExitThenAbandon"], reducing retry to 0
        // makes the template-matching loop fail immediately after 1 attempt (~0.5s instead of ~11s).
        // retry_times will be restored when ExitThenAbandon starts (via RestoreRetry hook).
        m_need_restore_retry = true;
        m_task_ptr->set_retry_times(0);
    }

    return true;
}


std::vector<std::string> asst::RoguelikeCustomStartTaskPlugin::get_select_list() const
{
    if (m_config->get_mode() != RoguelikeMode::Collectible ||
        m_config->get_run_for_collectible() /* 正在烧水，使用默认策略 */ ||
        m_config->get_only_start_with_elite_two() /* 只凹精二没有奖励，但第一次开时可能有之前的奖励 */) {
        return {};
    }

    std::vector<std::string> list;
    if (m_start_select.hot_water) {
        list.emplace_back(m_config->get_theme() + "@Roguelike@LastReward"); // 热水壶
    }
    if (m_start_select.shield) {
        list.emplace_back(m_config->get_theme() + "@Roguelike@LastReward2"); // 盾；傀影没盾，是生命
    }
    if (m_start_select.ingot) {
        list.emplace_back(m_config->get_theme() + "@Roguelike@LastReward3"); // 源石锭
    }
    if (m_start_select.hope) {
        list.emplace_back(m_config->get_theme() + "@Roguelike@LastReward4"); // 希望
    }

    if (m_start_select.random) {
        list.emplace_back(m_config->get_theme() + "@Roguelike@LastRewardRand"); // 随机奖励
    }
    if (m_config->get_theme() == RoguelikeTheme::Mizuki) {
        if (m_start_select.key) {
            list.emplace_back("Mizuki@Roguelike@LastReward5"); // 钥匙
        }
        if (m_start_select.dice) {
            list.emplace_back("Mizuki@Roguelike@LastReward6"); // 骰子
        }
    }
    else if (m_config->get_theme() == RoguelikeTheme::Sarkaz && m_start_select.ideas) {
        list.emplace_back("Sarkaz@Roguelike@LastReward5"); // 构想
    }
    else if (m_config->get_theme() == RoguelikeTheme::JieGarden && m_start_select.ticket) {
        list.emplace_back("JieGarden@Roguelike@LastReward5"); // 票券
    }

    return list;
}
