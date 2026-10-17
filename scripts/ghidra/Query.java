// Ad-hoc queries against an analyzed program; results go to <outfile>.
// Usage (headless): -postScript Query.java <outfile> <mode> <arg>
//   decompile <address|name>   decompiled C for one function
//   xrefs     <address|name>   references to an address/function, with containing function
//   strrefs   <substring>      strings containing substring, with referencing functions
//   callees   <address|name>   functions called by a function
// @category Switch

import java.io.File;
import java.io.PrintWriter;
import java.util.List;
import java.util.Set;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;

public class Query extends GhidraScript {

    private PrintWriter out;

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 3) {
            printerr("usage: Query.java <outfile> <decompile|xrefs|strrefs|callees> <arg>");
            return;
        }
        try (PrintWriter w = new PrintWriter(new File(args[0]), "UTF-8")) {
            out = w;
            out.println("# " + currentProgram.getName() + " " + args[1] + " " + args[2]);
            switch (args[1]) {
                case "decompile" -> decompile(resolve(args[2]));
                case "xrefs" -> xrefs(args[2]);
                case "strrefs" -> strrefs(args[2]);
                case "callees" -> callees(resolve(args[2]));
                case "closure" -> closure(resolve(args[2]));
                default -> out.println("unknown mode " + args[1]);
            }
        }
        println("Query written to " + args[0]);
    }

    private Address resolveAddress(String s) {
        List<Symbol> syms = getSymbols(s, null);
        if (!syms.isEmpty()) return syms.get(0).getAddress();
        return toAddr(s.startsWith("0x") ? s.substring(2) : s);
    }

    private Function resolve(String s) {
        Address a = resolveAddress(s);
        Function f = a == null ? null : getFunctionContaining(a);
        if (f == null && a != null) {                    // reached only through data (e.g. a runtime-filled table):
            disassemble(a);                              // make it here; read-only runs do not save it
            f = createFunction(a, null);
            if (f != null) out.println("(function created at " + a + " for this query)");
        }
        if (f == null) out.println("no function at " + s);
        return f;
    }

    private String where(Address a) {
        Function f = getFunctionContaining(a);
        return f == null ? "(no function)" : f.getName(true) + " @ " + f.getEntryPoint();
    }

    private void decompile(Function f) {
        if (f == null) return;
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        DecompileResults r = d.decompileFunction(f, 120, monitor);
        out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "decompile failed: " + r.getErrorMessage());
        d.dispose();
    }

    private void xrefs(String s) {
        // Names may live in any namespace (imports are under <EXTERNAL>::<lib>); callers reach
        // imports through thunk stubs, so report references to those stubs as well.
        Set<Address> targets = new java.util.LinkedHashSet<>();
        for (Symbol sym : currentProgram.getSymbolTable().getSymbols(s)) {
            targets.add(sym.getAddress());
            Function f = getFunctionAt(sym.getAddress());
            if (f == null && sym.getObject() instanceof Function ef) f = ef;
            if (f != null) {
                Address[] thunks = f.getFunctionThunkAddresses(true);
                if (thunks != null) targets.addAll(java.util.Arrays.asList(thunks));
            }
        }
        if (targets.isEmpty()) {
            Address a = resolveAddress(s);
            if (a == null) { out.println("unresolved"); return; }
            targets.add(a);
        }
        for (Address t : targets) {
            out.println("## " + t);
            for (Reference r : getReferencesTo(t)) {
                out.printf("%s\t%s\t%s%n", r.getFromAddress(), r.getReferenceType(), where(r.getFromAddress()));
            }
        }
    }

    private void strrefs(String needle) {
        String lower = needle.toLowerCase();
        DataIterator it = currentProgram.getListing().getDefinedData(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Data d = it.next();
            if (!d.hasStringValue()) continue;
            String v = String.valueOf(d.getValue());
            if (!v.toLowerCase().contains(lower)) continue;
            out.printf("%s\t\"%s\"%n", d.getAddress(), v.replace("\n", "\\n"));
            for (Reference r : getReferencesTo(d.getAddress())) {
                out.printf("    <- %s\t%s%n", r.getFromAddress(), where(r.getFromAddress()));
            }
        }
    }

    /** Everything reachable from f through direct calls: named functions, unresolved/indirect calls, SVCs. */
    private void closure(Function f) {
        if (f == null) return;
        java.util.Deque<Function> work = new java.util.ArrayDeque<>(List.of(f));
        Set<Function> seen = new java.util.LinkedHashSet<>();
        int svcs = 0, indirect = 0;
        while (!work.isEmpty() && seen.size() < 5000) {
            Function g = work.pop();
            if (!seen.add(g)) continue;
            for (ghidra.program.model.listing.Instruction ins : currentProgram.getListing().getInstructions(g.getBody(), true)) {
                String m = ins.getMnemonicString();
                if (m.equals("svc")) { svcs++; out.printf("SVC %s in %s%n", ins.getDefaultOperandRepresentation(0), g.getName(true)); }
                if (m.equals("blr") || m.equals("br")) indirect++;
            }
            for (Function c : g.getCalledFunctions(monitor)) if (!seen.contains(c)) work.push(c);
        }
        out.printf("closure of %s: %d functions, %d SVC instructions, %d indirect branches%n", f.getName(true), seen.size(), svcs, indirect);
        for (Function g : seen) if (!g.getName().startsWith("FUN_")) out.printf("  %s\t%s%n", g.getEntryPoint(), g.getName(true));
    }

    private void callees(Function f) {
        if (f == null) return;
        Set<Function> called = f.getCalledFunctions(monitor);
        for (Function c : called) out.printf("%s\t%s%n", c.getEntryPoint(), c.getName(true));
    }
}
