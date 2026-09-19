using Sempervirens;
using System.Text.Json;

if (args.Length == 2 && args[0] == "--default-profile")
{
    try
    {
        var loaded = ProfileLoader.LoadDefault(args[1]);
        Console.WriteLine(JsonSerializer.Serialize(new
        {
            valid = true, name = loaded.Name, ruleCount = loaded.Rules.Count
        }));
    }
    catch (Exception)
    {
        Console.WriteLine("{\"valid\":false}");
    }
    return 0;
}

if (args.Length == 2 && args[0] == "--profile")
{
    try
    {
        var loaded = ProfileLoader.LoadFromFile(args[1]);
        Console.WriteLine(JsonSerializer.Serialize(new
        {
            valid = true,
            schemaVersion = loaded.SchemaVersion,
            name = loaded.Name,
            description = loaded.Description,
            rules = loaded.Rules.Select(rule => new
            {
                id = rule.Id, group = rule.Group, name = rule.Name,
                description = rule.Description,
                kind = rule.ParsedKind.ToString().ToLowerInvariant(),
                builtin = rule.ParsedBuiltin?.ToString().ToLowerInvariant(),
                category = rule.Category ?? string.Empty, source = rule.Source, target = rule.Target,
                recursive = rule.Recursive, defaultSelected = rule.DefaultSelected,
                risk = rule.Risk
            }).ToArray()
        }));
    }
    catch (Exception)
    {
        Console.WriteLine("{\"valid\":false}");
    }
    return 0;
}

if (args.Length == 2 && args[0] == "--discover")
{
    var selection = new InstanceDiscovery().DiscoverSelection(args[1]);
    Console.WriteLine(JsonSerializer.Serialize(new
    {
        candidates = selection.Candidates.Select(item => new
        {
            name = item.DisplayName,
            root = item.Root,
            isolated = item.IsVersionIsolated,
            markers = item.MarkerCount,
            usable = item.IsUsable,
            options = item.HasOptions,
            servers = item.HasServers,
            screenshots = item.ScreenshotCount,
            worlds = item.WorldCount,
            note = item.AccessNote
        }).ToArray()
    }));
    return 0;
}

var execute = args.Length >= 4 && args[0] == "--execute";
if (!(args.Length == 3 || execute && (args.Length == 4 || args.Length == 5)))
{
    Console.Error.WriteLine("Usage: ParityReference [--execute [backup|replace|skip]] <profile> <source> <destination>");
    return 2;
}

var offset = execute ? args.Length == 5 ? 2 : 1 : 0;
var profile = ProfileLoader.LoadFromFile(args[offset]);
var core = new MigrationCore();
var plan = core.BuildPlan(profile, args[offset + 1], args[offset + 2], CancellationToken.None);
if (execute)
{
    var strategy = args.Length == 5 ? args[1] switch
    {
        "backup" => OverwriteStrategy.BackupAndReplace,
        "replace" => OverwriteStrategy.Replace,
        "skip" => OverwriteStrategy.Skip,
        _ => throw new ArgumentException("Unknown conflict strategy")
    } : OverwriteStrategy.BackupAndReplace;
    var migration = core.ExecuteMigration(plan, strategy, null,
        CancellationToken.None);
    Console.WriteLine(JsonSerializer.Serialize(new
    {
        success = migration.SuccessCount,
        skipped = migration.SkippedCount,
        failed = migration.FailedCount,
        copied = migration.FilesCopied,
        overwritten = migration.FilesOverwritten,
        backedUp = migration.FilesBackedUp,
        filesFailed = migration.FilesFailed,
        items = migration.Items.Select(item => new
        {
            name = item.Name,
            target = Path.GetRelativePath(migration.TargetRoot, item.TargetPath).Replace('\\', '/'),
            status = item.Status,
            copied = item.FilesCopied,
            overwritten = item.FilesOverwritten,
            backedUp = item.FilesBackedUp
        }).ToArray()
    }));
    return 0;
}
static string Relative(string root, string path) => Path.GetRelativePath(root, path).Replace('\\', '/');
var result = new
{
    valid = plan.IsValid,
    error = plan.Error ?? string.Empty,
    operations = plan.Operations.Select(operation => new
    {
        id = operation.Id,
        name = operation.Name,
        status = operation.Status.ToString(),
        selected = operation.Selected,
        source = Relative(plan.SourceRoot, operation.SourcePath),
        target = Relative(plan.TargetRoot, operation.TargetPath),
        size = operation.Size,
        directory = operation.IsDirectory,
        builtin = operation.IsBuiltin,
        difference = MigrationExplainer.Explain(operation, OverwriteStrategy.BackupAndReplace).Difference,
        settings = operation.Comparison is FileComparisonInfo file && file.Settings is not null
            ? file.Settings.Select(change => new
            {
                key = change.Key,
                target = change.TargetValue,
                source = change.SourceValue
            }).ToArray()
            : null
    }).ToArray()
};
Console.WriteLine(JsonSerializer.Serialize(result));
return 0;
