#include <windows.h>
#include <climits>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <fcntl.h>
#include <io.h>
#include <set>
#include <string>
#include <vector>
#include <limits>
#include "mcmf.h"

namespace {
struct InputArc { long tail, head, lower, upper; long long cost; };

struct Cs2Limits
{
    using Capacity = long;
    using Flow = long;
    using Cost = long long;
    using Objective = long long;

    static_assert(sizeof(Capacity) >= 4, "CS2 capacities require at least 32 bits");
    static_assert(sizeof(Cost) >= 8, "CS2 costs require at least 64 bits");
    static_assert(sizeof(Objective) >= sizeof(Cost), "objective must represent a cost");

    static bool acceptsDimensions(long nodes, long arcs)
    {
        constexpr long bucketScale = 12;
        const long maximumNodes = ((std::numeric_limits<long>::max)() - 2) / bucketScale - 1;
        return nodes > 0 && arcs >= 0 && nodes <= maximumNodes
            && arcs <= ((std::numeric_limits<long>::max)() - 1) / 2;
    }

    static bool acceptsCost(Cost value, long nodes)
    {
        // CS2 scales costs by n + 1 and can apply a bounded rank adjustment.
        constexpr Cost priceSafetyFactor = 64;
        if (value < 0 || nodes <= 0) return false;
        Cost maximum = (std::numeric_limits<Cost>::max)() / (static_cast<Cost>(nodes) + 1);
        maximum /= static_cast<Cost>(nodes) + 1;
        maximum /= priceSafetyFactor;
        return value <= maximum;
    }
};

bool multiplyWouldOverflow(long long left, long long right)
{
    return left != 0 && right > (std::numeric_limits<long long>::max)() / left;
}

bool addWouldOverflow(long long left, long long right)
{
    return right > 0 && left > (std::numeric_limits<long long>::max)() - right;
}

bool onlyWhitespaceRemains(const char* cursor)
{
    while (*cursor != '\0') {
        if (*cursor != ' ' && *cursor != '\t' && *cursor != '\r' && *cursor != '\n') return false;
        ++cursor;
    }
    return true;
}

bool parseProblem(const char* line, long& nodes, long& arcs)
{
    char kind[4] = {};
    int consumed = 0;
    return sscanf_s(line, "p %3s %ld %ld %n", kind, static_cast<unsigned>(_countof(kind)), &nodes, &arcs, &consumed) == 3
        && strcmp(kind, "min") == 0 && nodes > 0 && arcs >= 0 && onlyWhitespaceRemains(line + consumed);
}

bool parseNode(const char* line, long& node, long long& supply)
{
    int consumed = 0;
    return sscanf_s(line, "n %ld %lld %n", &node, &supply, &consumed) == 2 && onlyWhitespaceRemains(line + consumed);
}

bool parseArc(const char* line, InputArc& arc)
{
    int consumed = 0;
    return sscanf_s(line, "a %ld %ld %ld %ld %lld %n", &arc.tail, &arc.head, &arc.lower, &arc.upper, &arc.cost, &consumed) == 5
        && onlyWhitespaceRemains(line + consumed);
}

bool updateBalance(long long& value, long long delta)
{
    if ((delta > 0 && value > (std::numeric_limits<long long>::max)() - delta)
        || (delta < 0 && value < (std::numeric_limits<long long>::min)() - delta)) return false;
    value += delta;
    return true;
}

int writeSolution(const std::wstring& inputPath, const std::vector<InputArc>& arcs,
                  const std::vector<long>& flows)
{
    if (flows.size() != arcs.size()) return 70;

    const std::wstring outputPath = inputPath + L".sol";
    HANDLE outputHandle = CreateFileW(outputPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (outputHandle == INVALID_HANDLE_VALUE) return 73;
    const int outputDescriptor = _open_osfhandle(reinterpret_cast<intptr_t>(outputHandle), _O_WRONLY | _O_BINARY);
    if (outputDescriptor == -1) { CloseHandle(outputHandle); DeleteFileW(outputPath.c_str()); return 73; }
    FILE* output = _fdopen(outputDescriptor, "wb");
    if (!output) { _close(outputDescriptor); DeleteFileW(outputPath.c_str()); return 73; }

    long long objective = 0;
    for (size_t i = 0; i < arcs.size(); ++i) {
        const long long flow = arcs[i].lower + flows[i];
        if (flow < arcs[i].lower || flow > arcs[i].upper || multiplyWouldOverflow(flow, arcs[i].cost)) { fclose(output); DeleteFileW(outputPath.c_str()); return 70; }
        const long long contribution = flow * arcs[i].cost;
        if (addWouldOverflow(objective, contribution)) { fclose(output); DeleteFileW(outputPath.c_str()); return 70; }
        objective += contribution;
    }
    if (fprintf(output, "c mcf solution\ns %lld\n", objective) < 0) { fclose(output); DeleteFileW(outputPath.c_str()); return 73; }
    for (size_t i = 0; i < arcs.size(); ++i) {
        const long long flow = arcs[i].lower + flows[i];
        if (flow != 0 && fprintf(output, "f %ld %ld %lld\n", arcs[i].tail, arcs[i].head, flow) < 0) { fclose(output); DeleteFileW(outputPath.c_str()); return 73; }
    }
    if (fprintf(output, "c end\n") < 0) { fclose(output); DeleteFileW(outputPath.c_str()); return 73; }
    if (fclose(output) != 0) { DeleteFileW(outputPath.c_str()); return 73; }
    return 0;
}
}

int wmain(int argc, wchar_t* argv[])
{
 if (argc != 2) return 64;
 std::wstring inputPath(argv[1]);
 if (inputPath.find(L'\0') != std::wstring::npos) return 64;
 FILE* input = nullptr;
 if (_wfopen_s(&input, inputPath.c_str(), L"rb") != 0 || !input) return 66;
 long nodes = 0, arcsExpected = 0; std::vector<long long> supply; std::vector<InputArc> arcs; std::set<long> suppliedNodes; char line[4096]; bool gotProblem = false;
 while (fgets(line, static_cast<int>(sizeof(line)), input)) {
  if (!strchr(line, '\n') && !feof(input)) { fclose(input); return 65; }
  const char type = line[0]; if (type == '\0' || type == '\n' || type == '\r' || type == 'c') continue;
  if (type == 'p') {
   if (gotProblem || !parseProblem(line, nodes, arcsExpected)) { fclose(input); return 65; }
   if (!Cs2Limits::acceptsDimensions(nodes, arcsExpected)) { fclose(input); return 65; }
   gotProblem = true; supply.assign(static_cast<size_t>(nodes) + 1, 0); arcs.reserve(static_cast<size_t>(arcsExpected));
  }
  else if (type == 'n') {
   long id = 0; long long amount = 0;
   if (!gotProblem || !parseNode(line, id, amount) || id < 1 || id > nodes || amount < LONG_MIN || amount > LONG_MAX || !suppliedNodes.insert(id).second) { fclose(input); return 65; }
   supply[id] = amount;
  }
  else if (type == 'a') {
   InputArc arc = {};
   if (!gotProblem || !parseArc(line, arc) || arc.tail < 1 || arc.tail > nodes || arc.head < 1 || arc.head > nodes || arc.lower < 0 || arc.upper < arc.lower || !Cs2Limits::acceptsCost(arc.cost, nodes) || arcs.size() == static_cast<size_t>(arcsExpected)) { fclose(input); return 65; }
   arcs.push_back(arc);
  }
  else { fclose(input); return 65; }
 }
 if (ferror(input) || fclose(input) != 0 || !gotProblem || (long)arcs.size() != arcsExpected) return 65;
 long long balance = 0; for (long id = 1; id <= nodes; ++id) if (!updateBalance(balance, supply[id])) return 65; if (balance != 0) return 65;

 if (arcs.empty()) {
  for (long id = 1; id <= nodes; ++id) if (supply[id] != 0) return 70;
  return writeSolution(inputPath, arcs, {});
 }

	const std::vector<long long> originalSupply = supply;
 MCMF_CS2 solver(nodes, (long)arcs.size());
 for (const InputArc& arc : arcs) { solver.set_arc(arc.tail, arc.head, arc.lower, arc.upper, arc.cost); if (!updateBalance(supply[arc.tail], -arc.lower) || !updateBalance(supply[arc.head], arc.lower)) return 65; }
 for (long id = 1; id <= nodes; ++id) { if (supply[id] < LONG_MIN || supply[id] > LONG_MAX) return 65; solver.set_supply_demand_of_node(id, (long)supply[id]); }
 if (solver.run_cs2() != 0) return 70;
 std::vector<long> flows; solver.get_forward_flows(flows); if (flows.size() != arcs.size()) return 70;
	std::vector<long long> solvedBalance = originalSupply;
	for (size_t i = 0; i < arcs.size(); ++i)
	{
		const long long flow = arcs[i].lower + flows[i];
		if (flow < arcs[i].lower || flow > arcs[i].upper ||
			!updateBalance(solvedBalance[arcs[i].tail], -flow) ||
			!updateBalance(solvedBalance[arcs[i].head], flow)) return 70;
	}
	for (long id = 1; id <= nodes; ++id) if (solvedBalance[id] != 0) return 70;
 return writeSolution(inputPath, arcs, flows);
}
