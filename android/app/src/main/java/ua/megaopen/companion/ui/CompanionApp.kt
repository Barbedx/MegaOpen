package ua.megaopen.companion.ui

import androidx.annotation.StringRes
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.MoreHoriz
import androidx.compose.material.icons.filled.RecordVoiceOver
import androidx.compose.material.icons.filled.Tune
import androidx.compose.material3.Icon
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.NavigationRail
import androidx.compose.material3.NavigationRailItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.res.stringResource
import androidx.navigation.NavHostController
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import ua.megaopen.companion.AppGraph
import ua.megaopen.companion.R

private enum class Tab(val route: String, @StringRes val label: Int, val icon: ImageVector) {
    Home("home", R.string.tab_home, Icons.Filled.Home),
    Voice("voice", R.string.tab_voice, Icons.Filled.RecordVoiceOver),
    Board("board", R.string.tab_board, Icons.Filled.Tune),
    More("more", R.string.tab_more, Icons.Filled.MoreHoriz),
}

/** The app's frame: a rail on a head unit's wide screen, a bar on a phone. */
@Composable
fun CompanionApp(graph: AppGraph) {
    val nav = rememberNavController()
    val wide = LocalConfiguration.current.screenWidthDp >= 600
    val here by nav.currentBackStackEntryAsState()
    val route = here?.destination?.route ?: Tab.Home.route
    val go: (String) -> Unit = { r ->
        nav.navigate(r) {
            popUpTo(Tab.Home.route) { saveState = true }
            launchSingleTop = true
            restoreState = true
        }
    }
    if (wide) {
        Row(Modifier.fillMaxSize()) {
            NavigationRail {
                for (t in Tab.entries) NavigationRailItem(selected = route == t.route, onClick = { go(t.route) },
                    icon = { Icon(t.icon, null) }, label = { Text(stringResource(t.label)) })
            }
            Screens(nav, graph, go, Modifier.fillMaxSize())
        }
    } else {
        Scaffold(bottomBar = {
            NavigationBar {
                for (t in Tab.entries) NavigationBarItem(selected = route == t.route, onClick = { go(t.route) },
                    icon = { Icon(t.icon, null) }, label = { Text(stringResource(t.label)) })
            }
        }) { pad -> Screens(nav, graph, go, Modifier.padding(pad).fillMaxSize()) }
    }
}

@Composable
private fun Screens(nav: NavHostController, graph: AppGraph, go: (String) -> Unit, modifier: Modifier) {
    NavHost(nav, startDestination = Tab.Home.route, modifier = modifier) {
        composable(Tab.Home.route) { HomeScreen(graph, openBoard = { go(Tab.Board.route) }) }
        composable(Tab.Voice.route) { VoiceScreen(graph) }
        composable(Tab.Board.route) { BoardScreen(graph) }
        composable(Tab.More.route) { MoreScreen(graph) }
    }
}
