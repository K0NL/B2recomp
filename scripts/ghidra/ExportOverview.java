// Export a program overview plus full function and string lists for offline review.
// Usage (headless): -postScript ExportOverview.java <output directory>
// @category Switch

import java.io.File;
import java.io.PrintWriter;
import java.util.Map;
import java.util.TreeMap;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

public class ExportOverview extends GhidraScript {

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        File outDir = new File(args.length > 0 ? args[0] : ".");
        outDir.mkdirs();
        String name = currentProgram.getName();

        try (PrintWriter o = new PrintWriter(new File(outDir, name + "-overview.txt"), "UTF-8")) {
            o.println("program: " + name);
            o.println("format: " + currentProgram.getExecutableFormat());
            o.println("language: " + currentProgram.getLanguageID() + " / " + currentProgram.getCompilerSpec().getCompilerSpecID());
            o.println("image_base: " + currentProgram.getImageBase());
            o.println("address_range: " + currentProgram.getMinAddress() + " - " + currentProgram.getMaxAddress());
            o.println("sha256: " + currentProgram.getExecutableSHA256());

            o.println("\n[memory blocks]");
            for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
                o.printf("%-16s %s-%s size=0x%x %s%s%s %s%n", b.getName(), b.getStart(), b.getEnd(), b.getSize(),
                    b.isRead() ? "r" : "-", b.isWrite() ? "w" : "-", b.isExecute() ? "x" : "-",
                    b.isInitialized() ? "" : "(uninitialized)");
            }

            int total = 0, unnamed = 0, thunks = 0, external = 0;
            try (PrintWriter f = new PrintWriter(new File(outDir, name + "-functions.tsv"), "UTF-8")) {
                f.println("address\tsize\tname\tcallers");
                FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
                while (it.hasNext() && !monitor.isCancelled()) {
                    Function fn = it.next();
                    total++;
                    if (fn.getName().startsWith("FUN_")) unnamed++;
                    if (fn.isThunk()) thunks++;
                    f.printf("%s\t%d\t%s\t%d%n", fn.getEntryPoint(), fn.getBody().getNumAddresses(),
                        fn.getName(true), fn.getSymbol().getReferenceCount());
                }
            }
            FunctionIterator ext = currentProgram.getFunctionManager().getExternalFunctions();
            try (PrintWriter f = new PrintWriter(new File(outDir, name + "-imports.txt"), "UTF-8")) {
                while (ext.hasNext()) { f.println(ext.next().getName(true)); external++; }
            }
            o.println("\n[functions]");
            o.printf("total=%d auto_named(FUN_)=%d named=%d thunks=%d external_imports=%d%n",
                total, unnamed, total - unnamed, thunks, external);

            Map<String, Integer> bySource = new TreeMap<>();
            SymbolIterator syms = currentProgram.getSymbolTable().getAllSymbols(false);
            while (syms.hasNext()) {
                Symbol s = syms.next();
                bySource.merge(s.getSource().toString(), 1, Integer::sum);
            }
            o.println("\n[symbols by source]");
            bySource.forEach((k, v) -> o.println(k + "=" + v));

            int strings = 0;
            try (PrintWriter f = new PrintWriter(new File(outDir, name + "-strings.tsv"), "UTF-8")) {
                f.println("address\tblock\tvalue");
                DataIterator it = currentProgram.getListing().getDefinedData(true);
                while (it.hasNext() && !monitor.isCancelled()) {
                    Data d = it.next();
                    if (!d.hasStringValue()) continue;
                    Object v = d.getValue();
                    if (v == null) continue;
                    Address a = d.getAddress();
                    MemoryBlock b = currentProgram.getMemory().getBlock(a);
                    String s = v.toString().replace("\t", "\\t").replace("\r", "\\r").replace("\n", "\\n");
                    f.printf("%s\t%s\t%s%n", a, b == null ? "?" : b.getName(), s);
                    strings++;
                }
            }
            o.println("\n[strings]");
            o.println("defined_strings=" + strings);
        }
        println("Overview written to " + outDir.getAbsolutePath());
    }
}
